#include "conversation/session.h"
#include "common/types.h"
#include "common/util.h"
#include "conversation/format.h"
#include "providers/pricing.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <string>
#include <utility>

namespace imza {

ModalPayload Session::modal() const
{
    std::lock_guard lock(_mutex);
    return _modal;
}

std::uint64_t Session::modal_serial() const
{
    std::lock_guard lock(_mutex);
    return _modal_serial;
}

std::uint64_t Session::content_serial() const
{
    std::lock_guard lock(_mutex);
    return _content_serial;
}

std::string Session::session_id() const
{
    std::lock_guard lock(_mutex);
    return _session_id;
}

Session::Phase Session::phase() const
{
    std::lock_guard lock(_mutex);
    return _phase;
}

Session::Mode Session::mode() const
{
    std::lock_guard lock(_mutex);
    return _mode;
}

std::string Session::error() const
{
    std::lock_guard lock(_mutex);
    return _error;
}

std::string Session::connect_status() const
{
    std::lock_guard lock(_mutex);
    return _connect_status;
}

std::optional<Session::Countdown> Session::retry_countdown() const
{
    std::lock_guard lock(_mutex);
    return _retry_countdown;
}

Usage Session::last() const
{
    std::lock_guard lock(_mutex);
    return _last;
}

std::optional<std::chrono::milliseconds> Session::turn_elapsed() const
{
    std::lock_guard lock(_mutex);
    if (!_turn_started) {
        return std::nullopt;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - *_turn_started);
}

Session::StatusView Session::status_view() const
{
    std::lock_guard lock(_mutex);
    return { _mode, _totals, _last, _total_cost };
}

bool Session::has_items() const
{
    std::lock_guard lock(_mutex);
    return !_items.empty();
}

bool Session::has_pending_work() const
{
    std::lock_guard lock(_mutex);
    if (_phase != Phase::IDLE || !_queued.empty()) {
        return true;
    }
    return std::any_of(
        _items.begin(), _items.end(), [](const ConversationItem& item) {
            const auto* tool = std::get_if<ToolCall>(&item);
            return tool != nullptr && !tool->result.has_value();
        });
}

SessionSnapshot Session::snapshot() const
{
    std::lock_guard lock(_mutex);
    return { _title, _items, _todo, _compacted_summary, _compacted_item_count,
        _mode == Mode::PLAN, _persistence };
}

std::optional<SessionSnapshot> Session::snapshot_for_save() const
{
    std::lock_guard lock(_mutex);
    if (_items.empty() || !_dirty) {
        return std::nullopt;
    }
    return SessionSnapshot { _title, _items, _todo, _compacted_summary,
        _compacted_item_count, _mode == Mode::PLAN, _persistence };
}

void Session::restore(SessionSnapshot snapshot)
{
    {
        std::lock_guard lock(_mutex);
        _title                = std::move(snapshot.title);
        _items                = std::move(snapshot.items);
        _todo                 = std::move(snapshot.todo);
        _compacted_summary    = std::move(snapshot.compacted_summary);
        _compacted_item_count = snapshot.compacted_item_count;
        _persistence          = std::move(snapshot.persistence);
        // A loaded session continues its source file in place and adopts the
        // file stem as its id. Only fresh conversations (/new, brand-new
        // objects) rotate to a generated id so their first save gains a new
        // file instead of overwriting an archive.
        if (auto* persisted = std::get_if<PersistedSession>(&_persistence)) {
            _session_id = persisted->path.stem().string();
        } else {
            _session_id = unique_session_id();
        }
        _dirty = false;
        _mode  = snapshot.plan_mode ? Mode::PLAN : Mode::BUILD;
        _modal = std::monostate { };
        std::vector<QueuedMessage>().swap(_queued);
        _error.clear();
        _retry_countdown.reset();
        _reasoning_start.reset();
        _turn_started.reset();
        _phase                    = Phase::IDLE;
        _totals                   = { };
        _last                     = { };
        _total_cost               = 0.0;
        _title_generation_claimed = !_title.empty();
        _interrupt_requested.store(false);
        _next_tool_id       = 1;
        _next_compaction_id = 1;
        for (const auto& item : _items) {
            if (const auto* tool = std::get_if<ToolCall>(&item)) {
                _next_tool_id = std::max(_next_tool_id, tool->id + 1);
            } else if (const auto* event
                = std::get_if<CompactionEvent>(&item)) {
                _next_compaction_id
                    = std::max(_next_compaction_id, event->id + 1);
            }
        }
        ++_modal_serial;
        ++_content_serial;
    }
    attachments_changed_.publish();
}

void Session::set_persistence(SessionPersistence persistence)
{
    std::lock_guard lock(_mutex);
    _persistence = std::move(persistence);
    // The file now mirrors the conversation until the next mutation.
    _dirty = false;
}

void Session::set_mode(Mode next_mode)
{
    std::lock_guard lock(_mutex);
    _mode = next_mode;
}

void Session::set_error(std::string msg)
{
    std::lock_guard lock(_mutex);
    _error = std::move(msg);
}

void Session::clear_error()
{
    std::lock_guard lock(_mutex);
    _error.clear();
}

void Session::set_connect_status(std::string status)
{
    std::lock_guard lock(_mutex);
    _connect_status = std::move(status);
}

std::string Session::title() const
{
    std::lock_guard lock(_mutex);
    return _title;
}

std::vector<std::string> Session::attachment_names() const
{
    std::lock_guard lock(_mutex);
    std::vector<std::string> names;
    for (const ConversationItem& item : _items) {
        const auto* user = std::get_if<UserTurn>(&item);
        if (user == nullptr) {
            continue;
        }
        for (const Attachment& attachment : user->attachments) {
            std::string name
                = utf8_from_path(path_from_utf8(attachment.path).filename());
            if (!name.empty()
                && std::find(names.begin(), names.end(), name) == names.end()) {
                names.push_back(std::move(name));
            }
        }
    }
    return names;
}

bool Session::claim_title_generation()
{
    std::lock_guard lock(_mutex);
    if (_title_generation_claimed || !_items.empty()) {
        return false;
    }
    _title_generation_claimed = true;
    return true;
}

void Session::set_title(std::string title)
{
    {
        std::lock_guard lock(_mutex);
        if (_title.empty()) {
            _title = std::move(title);
            _dirty = true;
        }
    }
    _notify_title_change();
}

void Session::cancel_queued(std::size_t id)
{
    std::lock_guard lock(_mutex);
    for (auto it = _queued.begin(); it != _queued.end(); ++it) {
        if (it->id == id) {
            _queued.erase(it);
            return;
        }
    }
}

void Session::enqueue_message(
    std::string text, std::vector<Attachment> attachments)
{
    std::lock_guard lock(_mutex);
    _queued.push_back(QueuedMessage {
        _next_queued_id++, std::move(text), std::move(attachments) });
}

std::optional<QueuedMessage> Session::pop_queued()
{
    std::lock_guard lock(_mutex);
    if (_queued.empty()) {
        return std::nullopt;
    }
    QueuedMessage next = std::move(_queued.front());
    _queued.erase(_queued.begin());
    return next;
}

void Session::begin_send(std::string text, std::vector<Attachment> attachments)
{
    const bool has_attachments = !attachments.empty();
    {
        std::lock_guard lock(_mutex);
        _persistence = UnsavedSession { };
        _dirty       = true;
        _items.emplace_back(
            UserTurn { std::move(text), std::move(attachments) });
        _error.clear();
        _phase        = Phase::CONNECTING;
        _turn_started = std::chrono::steady_clock::now();
    }
    if (has_attachments) {
        attachments_changed_.publish();
    }
}

void Session::append_assistant(std::string model, std::string reasoning_effort)
{
    std::lock_guard lock(_mutex);
    _dirty = true;
    _items.emplace_back(AssistantTurn { .model = std::move(model),
        .reasoning_effort                      = std::move(reasoning_effort) });
}

void Session::set_last_assistant_metadata(
    std::string model, std::string reasoning_effort)
{
    std::lock_guard lock(_mutex);
    if (AssistantTurn* assistant = last_assistant_locked()) {
        assistant->model            = std::move(model);
        assistant->reasoning_effort = std::move(reasoning_effort);
    }
}

void Session::append_item(ConversationItem item)
{
    std::lock_guard lock(_mutex);
    _dirty = true;
    _items.push_back(std::move(item));
}

std::pair<std::size_t, std::size_t> Session::begin_compaction()
{
    std::lock_guard lock(_mutex);
    const std::size_t id = _next_compaction_id++;
    std::size_t prefix   = 0;
    for (std::size_t index = _items.size(); index > 0; --index) {
        if (std::holds_alternative<UserTurn>(_items[index - 1])) {
            prefix = index - 1;
            break;
        }
    }
    _items.emplace_back(
        CompactionEvent { id, CompactionEvent::Status::RUNNING });
    return { id, prefix };
}

void Session::finish_compaction(std::size_t id, std::string summary,
    std::size_t compacted_item_count, bool success)
{
    std::lock_guard lock(_mutex);
    for (auto& item : _items) {
        auto* event = std::get_if<CompactionEvent>(&item);
        if (event == nullptr || event->id != id) {
            continue;
        }
        event->status = success ? CompactionEvent::Status::COMPLETED
                                : CompactionEvent::Status::FAILED;
        _dirty        = true;
        if (success) {
            _compacted_summary    = std::move(summary);
            _compacted_item_count = compacted_item_count;
        }
        return;
    }
}

void Session::append_tool(const ToolCallRequest& req)
{
    std::lock_guard lock(_mutex);
    if (auto* a = last_assistant_locked()) {
        finalize_reasoning(*a);
    }
    _dirty               = true;
    const std::size_t id = _next_tool_id++;
    _items.emplace_back(
        ToolCall { id, req.id, req.name, req.args, { }, { }, std::nullopt });
}

ToolCall* Session::_find_tool_locked(
    const ToolCallRequest& req, const bool unfinished_only)
{
    for (auto it = _items.rbegin(); it != _items.rend(); ++it) {
        auto* call = std::get_if<ToolCall>(&*it);
        if (call == nullptr || (unfinished_only && call->result.has_value())) {
            continue;
        }
        const bool matched = !req.id.empty()
            ? call->call_id == req.id
            : call->name == req.name && call->args == req.args;
        if (matched) {
            return call;
        }
    }
    return nullptr;
}

ToolCall* Session::find_planning_tool_locked(const ToolCallRequest& req)
{
    for (auto it = _items.rbegin(); it != _items.rend(); ++it) {
        auto* call = std::get_if<ToolCall>(&*it);
        if (call == nullptr || call->phase != ToolCall::Phase::PLANNING) {
            continue;
        }
        const bool matched = !req.id.empty() ? call->call_id == req.id
                                             : call->name == req.name;
        if (matched) {
            return call;
        }
    }
    return nullptr;
}

void Session::set_tool_subagent_chats(
    const ToolCallRequest& req, std::vector<SubagentChat> chats)
{
    std::lock_guard lock(_mutex);
    if (auto* call = _find_tool_locked(req, false)) {
        call->subagent_chats = std::move(chats);
        ++_content_serial;
    }
}

void Session::set_tool_subagents(
    const ToolCallRequest& req, std::vector<std::size_t> ids)
{
    std::lock_guard lock(_mutex);
    if (auto* call = _find_tool_locked(req, true)) {
        call->subagent_ids = std::move(ids);
        ++_content_serial;
    }
}

void Session::fill_tool_result(
    const ToolCallRequest& req, ToolCall::Result result)
{
    std::lock_guard lock(_mutex);
    if (auto* call = _find_tool_locked(req, true)) {
        call->result = std::move(result);
        _dirty       = true;
    }
}

void Session::set_todo(TodoList todo)
{
    std::lock_guard lock(_mutex);
    if (todo != _todo) {
        _dirty = true;
    }
    _todo = std::move(todo);
}

void Session::set_modal(ModalPayload payload)
{
    std::lock_guard lock(_mutex);
    _modal = std::move(payload);
}

void Session::clear_modal()
{
    std::lock_guard lock(_mutex);
    _modal = std::monostate { };
}

void Session::bump_modal_serial()
{
    std::lock_guard lock(_mutex);
    ++_modal_serial;
}

void Session::present_modal(ModalPayload payload)
{
    std::lock_guard lock(_mutex);
    _modal = std::move(payload);
    if (std::holds_alternative<PermissionPrompt>(_modal)
        || std::holds_alternative<QuestionForm>(_modal)) {
        _phase = Phase::AWAITING;
    }
    ++_modal_serial;
}

void Session::set_phase(Phase phase)
{
    std::lock_guard lock(_mutex);
    _phase = phase;
}

void Session::mark_retry(int wait_seconds, bool stalled)
{
    std::lock_guard lock(_mutex);
    _phase           = Phase::CONNECTING;
    _retry_countdown = Countdown { std::chrono::steady_clock::now()
            + std::chrono::seconds(wait_seconds),
        stalled };
}

void Session::reset_reasoning()
{
    std::lock_guard lock(_mutex);
    _reasoning_start = std::chrono::steady_clock::now();
}

void Session::request_interrupt() { _interrupt_requested.store(true); }

void Session::clear_interrupt() { _interrupt_requested.store(false); }

bool Session::interrupt_requested() const
{
    return _interrupt_requested.load();
}

std::optional<AssistantTurn> Session::last_assistant() const
{
    std::lock_guard lock(_mutex);
    if (const auto* assistant = last_assistant_locked()) {
        return *assistant;
    }
    return std::nullopt;
}

bool Session::finish_session(std::string error)
{
    std::lock_guard lock(_mutex);
    const bool finished = _phase != Phase::IDLE;
    finish_session_locked(error);
    return finished;
}

std::vector<Message> Session::build_history(
    std::string_view system_prompt, ApiStandard dialect) const
{
    std::lock_guard lock(_mutex);
    std::vector<Message> history;
    history.push_back({ Message::Type::SYSTEM, std::string(system_prompt) });
    if (!_compacted_summary.empty()) {
        history.push_back({ Message::Type::USER,
            "<session-summary>\n" + _compacted_summary
                + "\n</session-summary>" });
    }
    const std::size_t begin = std::min(_compacted_item_count, _items.size());
    for (std::size_t index = begin; index < _items.size(); ++index) {
        const auto& item = _items[index];
        if (const auto* u = std::get_if<UserTurn>(&item)) {
            Message user { Message::Type::USER,
                message_with_attachments(u->text, u->attachments) };
            for (const Attachment& attachment : u->attachments) {
                if (attachment.type != Attachment::Type::TEXT) {
                    user.media.push_back(attachment);
                }
            }
            history.push_back(std::move(user));
        } else if (const auto* a = std::get_if<AssistantTurn>(&item)) {
            history.push_back(assistant_message(a->markdown, a, dialect));
        } else if (const auto* tc = std::get_if<ToolCall>(&item)) {
            if (tc->phase == ToolCall::Phase::PLANNING) {
                continue;
            }
            if (history.empty()
                || history.back().type != Message::Type::ASSISTANT) {
                history.push_back({ Message::Type::ASSISTANT, "" });
            }
            history.back().tool_calls.push_back(
                ToolCallEntry { tc->call_id, tc->name, tc->args });
            history.push_back({ Message::Type::TOOL, tool_result_text(*tc), { },
                tc->call_id });
        }
    }

    return history;
}

void Session::apply(const StreamEvent& ev, const ModelPricing& pricing)
{
    std::lock_guard lock(_mutex);
    switch (ev.kind) {
    case StreamEvent::Kind::CONTENT_DELTA:
        assert(_phase != Phase::AWAITING);
        if (!_items.empty()) {
            if (auto* a = std::get_if<AssistantTurn>(&_items.back())) {
                if (!ev.text.empty()) {
                    finalize_reasoning(*a);
                    _dirty = true;
                }
                a->markdown += ev.text;
            }
        }
        break;
    case StreamEvent::Kind::REASONING:
        if (!_items.empty()) {
            if (auto* a = std::get_if<AssistantTurn>(&_items.back())) {
                if (a->reasoning.empty() && !_reasoning_start.has_value()) {
                    _reasoning_start = std::chrono::steady_clock::now();
                }
                if (!ev.text.empty()) {
                    a->reasoning += ev.text;
                    _dirty = true;
                }
                if (!ev.thinking_signature.empty()) {
                    a->reasoning_signature = ev.thinking_signature;
                }
            }
        }
        break;
    case StreamEvent::Kind::TOOL_CALL_START:
        if (auto* a = last_assistant_locked()) {
            finalize_reasoning(*a);
        }
        // Transient: arguments are still streaming, so the item is not
        // marked dirty and is never persisted or sent back as history.
        _items.emplace_back(
            ToolCall { _next_tool_id++, ev.tool_call.id, ev.tool_call.name, "",
                { }, { }, std::nullopt, ToolCall::Phase::PLANNING });
        break;
    case StreamEvent::Kind::TOOL_CALL:
        if (auto* a = last_assistant_locked()) {
            finalize_reasoning(*a);
        }
        if (auto* planned = find_planning_tool_locked(ev.tool_call)) {
            planned->name  = ev.tool_call.name;
            planned->args  = ev.tool_call.args;
            planned->phase = ToolCall::Phase::EXECUTING;
        } else {
            _items.emplace_back(ToolCall { _next_tool_id++, ev.tool_call.id,
                ev.tool_call.name, ev.tool_call.args, { }, { }, std::nullopt });
        }
        _dirty = true;
        break;
    case StreamEvent::Kind::QUESTION:
        if (!_items.empty()) {
            if (auto* a = std::get_if<AssistantTurn>(&_items.back())) {
                if (!a->markdown.empty()) {
                    a->markdown += "\n\n";
                }
                a->markdown += question_form_markdown(ev.question);
                _dirty = true;
            }
        }
        break;
    case StreamEvent::Kind::DONE:
        if (auto* a = last_assistant_locked()) {
            finalize_reasoning(*a);
        }
        break;
    case StreamEvent::Kind::ERROR:
        finish_session_locked(error_text(ev.error));
        break;
    case StreamEvent::Kind::CONNECTED:
        if (_phase == Phase::CONNECTING) {
            _error.clear();
            _retry_countdown.reset();
            _phase = Phase::STREAMING;
        }
        break;
    case StreamEvent::Kind::USAGE: update_usage(ev, pricing); break;
    }
}

AssistantTurn* Session::last_assistant_locked()
{
    return const_cast<AssistantTurn*>(
        static_cast<const Session*>(this)->last_assistant_locked());
}

const AssistantTurn* Session::last_assistant_locked() const
{
    for (auto it = _items.rbegin(); it != _items.rend(); ++it) {
        if (const auto* a = std::get_if<AssistantTurn>(&*it)) {
            return a;
        }
    }
    return nullptr;
}

void Session::finalize_reasoning(AssistantTurn& a)
{
    if (!_reasoning_start.has_value() || a.reasoning_ms.has_value()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    a.reasoning_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - *_reasoning_start);
}

void Session::finish_session_locked(const std::string& error)
{
    if (_phase == Phase::IDLE) {
        return;
    }
    if (auto* a = last_assistant_locked()) {
        finalize_reasoning(*a);
    }
    _retry_countdown.reset();
    std::erase_if(_items, [](const ConversationItem& item) {
        const auto* tool = std::get_if<ToolCall>(&item);
        return tool != nullptr && tool->phase == ToolCall::Phase::PLANNING;
    });
    if (!error.empty() && _error.empty()) {
        _error = error;
    }
    _phase = Phase::IDLE;
}

void Session::update_usage(
    const StreamEvent& usage_event, const ModelPricing& pricing)
{
    _last = usage_event.usage;
    _totals.prompt += usage_event.usage.prompt;
    _totals.completion += usage_event.usage.completion;
    _totals.total += usage_event.usage.total;
    _total_cost += compute_cost(usage_event.usage, pricing);
}

Signal<>::Subscription Session::subscribe_to_title_change(
    Signal<>::Callback callback)
{
    return title_changed_.subscribe(std::move(callback));
}

Signal<>::Subscription Session::subscribe_to_attachments_change(
    Signal<>::Callback callback)
{
    return attachments_changed_.subscribe(std::move(callback));
}

void Session::_notify_title_change() { title_changed_.publish(); }

WorkflowPhase next_workflow_phase(WorkflowPhase phase, bool review_available)
{
    switch (phase) {
    case WorkflowPhase::PLAN: return WorkflowPhase::BUILD;
    case WorkflowPhase::BUILD:
        return review_available ? WorkflowPhase::REVIEW : WorkflowPhase::PLAN;
    case WorkflowPhase::REVIEW: return WorkflowPhase::PLAN;
    }
    return WorkflowPhase::PLAN;
}

WorkflowPhase previous_workflow_phase(
    WorkflowPhase phase, bool review_available)
{
    switch (phase) {
    case WorkflowPhase::PLAN:
        return review_available ? WorkflowPhase::REVIEW : WorkflowPhase::BUILD;
    case WorkflowPhase::BUILD: return WorkflowPhase::PLAN;
    case WorkflowPhase::REVIEW: return WorkflowPhase::BUILD;
    }
    return WorkflowPhase::PLAN;
}

std::optional<Session::Mode> workflow_mode(WorkflowPhase phase)
{
    switch (phase) {
    case WorkflowPhase::PLAN: return Session::Mode::PLAN;
    case WorkflowPhase::BUILD: return Session::Mode::BUILD;
    case WorkflowPhase::REVIEW: return std::nullopt;
    }
    return std::nullopt;
}

} // namespace imza
