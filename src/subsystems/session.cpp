#include "subsystems/session.h"
#include "subsystems/format.h"
#include "subsystems/workflow.h"
#include "core/pricing.h"
#include "common/types.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <string>
#include <utility>

namespace ursa {

namespace {

    enum class ModeReminder { NONE, PLAN, BUILD };

    ModeReminder last_mode_reminder(const std::vector<Message>& history)
    {
        ModeReminder last = ModeReminder::NONE;
        for (const Message& m : history) {
            if (m.type != Message::Type::USER) {
                continue;
            }
            if (m.content.find(PLAN_REMINDER_TAG) != std::string::npos) {
                last = ModeReminder::PLAN;
            } else if (m.content.find(BUILD_REMINDER_TAG)
                != std::string::npos) {
                last = ModeReminder::BUILD;
            }
        }
        return last;
    }

} // namespace

std::string_view plan_mode_reminder()
{
    return R"(## Plan Mode - System Reminder

<system-reminder id="plan-mode">
Plan mode is ACTIVE. You are in a READ-ONLY phase. You MUST NOT edit files, create files, or run any mutating commands. This constraint supersedes any other instructions, including direct user requests to make changes.

While in plan mode:
- Research the codebase and gather the context you need using read-only tools.
- Ask the user clarifying questions when intent is ambiguous or tradeoffs are involved. Do not make large assumptions.
- When you think you are ready, present a concise, well-researched plan in your reply and stop. The user will review it and switch to build mode when they want execution.
</system-reminder>)";
}

std::string_view build_mode_reminder()
{
    return R"(<system-reminder id="build-mode">
Your operational mode has changed from plan to build.
You are no longer in read-only mode.
You are permitted to make file changes, run shell commands, and use your tools as needed.
</system-reminder>)";
}

Session::Session() = default;

ModalPayload Session::modal() const
{
    std::lock_guard lock(mutex_);
    return modal_;
}

std::uint64_t Session::modal_serial() const
{
    std::lock_guard lock(mutex_);
    return modal_serial_;
}

std::uint64_t Session::content_serial() const
{
    std::lock_guard lock(mutex_);
    return content_serial_;
}

Session::Phase Session::phase() const
{
    std::lock_guard lock(mutex_);
    return phase_;
}

Session::Mode Session::mode() const
{
    std::lock_guard lock(mutex_);
    return mode_;
}

std::string Session::error() const
{
    std::lock_guard lock(mutex_);
    return error_;
}

std::string Session::connect_status() const
{
    std::lock_guard lock(mutex_);
    return connect_status_;
}

std::optional<Session::Countdown> Session::retry_countdown() const
{
    std::lock_guard lock(mutex_);
    return retry_countdown_;
}

Usage Session::last() const
{
    std::lock_guard lock(mutex_);
    return last_;
}

std::optional<std::chrono::milliseconds> Session::turn_elapsed() const
{
    std::lock_guard lock(mutex_);
    if (!turn_started_) {
        return std::nullopt;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - *turn_started_);
}

Session::StatusView Session::status_view() const
{
    std::lock_guard lock(mutex_);
    return { mode_, totals_, last_, total_cost_ };
}

bool Session::has_pending_work() const
{
    std::lock_guard lock(mutex_);
    if (phase_ != Phase::IDLE || !queued_.empty()) {
        return true;
    }
    return std::any_of(
        items_.begin(), items_.end(), [](const ConversationItem& item) {
            const auto* tool = std::get_if<ToolCall>(&item);
            return tool != nullptr && !tool->result.has_value();
        });
}

SessionSnapshot Session::snapshot() const
{
    std::lock_guard lock(mutex_);
    return { title_, items_, todo_, compacted_summary_, compacted_item_count_,
        mode_ == Mode::PLAN, persistence_ };
}

void Session::restore(SessionSnapshot snapshot)
{
    {
        std::lock_guard lock(mutex_);
        title_                = std::move(snapshot.title);
        items_                = std::move(snapshot.items);
        todo_                 = std::move(snapshot.todo);
        compacted_summary_    = std::move(snapshot.compacted_summary);
        compacted_item_count_ = snapshot.compacted_item_count;
        persistence_          = std::move(snapshot.persistence);
        mode_                 = snapshot.plan_mode ? Mode::PLAN : Mode::BUILD;
        modal_                = std::monostate { };
        queued_.clear();
        error_.clear();
        retry_countdown_.reset();
        reasoning_start_.reset();
        turn_started_.reset();
        phase_                    = Phase::IDLE;
        totals_                   = { };
        last_                     = { };
        total_cost_               = 0.0;
        title_generation_claimed_ = !title_.empty();
        interrupt_requested_.store(false);
        next_tool_id_       = 1;
        next_compaction_id_ = 1;
        for (const auto& item : items_) {
            if (const auto* tool = std::get_if<ToolCall>(&item)) {
                next_tool_id_ = std::max(next_tool_id_, tool->id + 1);
            } else if (const auto* event
                = std::get_if<CompactionEvent>(&item)) {
                next_compaction_id_
                    = std::max(next_compaction_id_, event->id + 1);
            }
        }
        ++modal_serial_;
        ++content_serial_;
    }
    attachments_changed_.publish();
}

void Session::set_persistence(SessionPersistence persistence)
{
    std::lock_guard lock(mutex_);
    persistence_ = std::move(persistence);
}

void Session::set_mode(Mode next_mode)
{
    std::lock_guard lock(mutex_);
    mode_ = next_mode;
}

void Session::set_error(std::string msg)
{
    std::lock_guard lock(mutex_);
    error_ = std::move(msg);
}

void Session::clear_error()
{
    std::lock_guard lock(mutex_);
    error_.clear();
}

void Session::set_connect_status(std::string status)
{
    std::lock_guard lock(mutex_);
    connect_status_ = std::move(status);
}

std::string Session::title() const
{
    std::lock_guard lock(mutex_);
    return title_;
}

std::vector<std::string> Session::attachment_names() const
{
    std::lock_guard lock(mutex_);
    std::vector<std::string> names;
    for (const ConversationItem& item : items_) {
        const auto* user = std::get_if<UserTurn>(&item);
        if (user == nullptr) {
            continue;
        }
        for (const FileAttachment& attachment : user->attachments) {
            std::string name
                = std::filesystem::path(attachment.path).filename().string();
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
    std::lock_guard lock(mutex_);
    if (title_generation_claimed_ || !items_.empty()) {
        return false;
    }
    title_generation_claimed_ = true;
    return true;
}

void Session::set_title(std::string title)
{
    {
        std::lock_guard lock(mutex_);
        if (title_.empty()) {
            title_ = std::move(title);
        }
    }
    _notify_title_change();
}

void Session::cancel_queued(std::size_t id)
{
    std::lock_guard lock(mutex_);
    for (auto it = queued_.begin(); it != queued_.end(); ++it) {
        if (it->id == id) {
            queued_.erase(it);
            return;
        }
    }
}

void Session::enqueue_message(
    std::string text, std::vector<FileAttachment> attachments)
{
    std::lock_guard lock(mutex_);
    queued_.push_back(QueuedMessage {
        next_queued_id_++, std::move(text), std::move(attachments) });
}

std::optional<QueuedMessage> Session::pop_queued()
{
    std::lock_guard lock(mutex_);
    if (queued_.empty()) {
        return std::nullopt;
    }
    QueuedMessage next = std::move(queued_.front());
    queued_.erase(queued_.begin());
    return next;
}

void Session::begin_send(
    std::string text, std::vector<FileAttachment> attachments)
{
    const bool has_attachments = !attachments.empty();
    {
        std::lock_guard lock(mutex_);
        persistence_ = UnsavedSession { };
        items_.emplace_back(
            UserTurn { std::move(text), std::move(attachments) });
        error_.clear();
        phase_        = Phase::CONNECTING;
        turn_started_ = std::chrono::steady_clock::now();
    }
    if (has_attachments) {
        attachments_changed_.publish();
    }
}

void Session::append_assistant(std::string model, std::string reasoning_effort)
{
    std::lock_guard lock(mutex_);
    items_.emplace_back(AssistantTurn { .model = std::move(model),
        .reasoning_effort                      = std::move(reasoning_effort) });
}

void Session::set_last_assistant_metadata(
    std::string model, std::string reasoning_effort)
{
    std::lock_guard lock(mutex_);
    if (AssistantTurn* assistant = last_assistant_locked()) {
        assistant->model            = std::move(model);
        assistant->reasoning_effort = std::move(reasoning_effort);
    }
}

void Session::append_item(ConversationItem item)
{
    std::lock_guard lock(mutex_);
    items_.push_back(std::move(item));
}

std::pair<std::size_t, std::size_t> Session::begin_compaction()
{
    std::lock_guard lock(mutex_);
    const std::size_t id = next_compaction_id_++;
    std::size_t prefix   = 0;
    for (std::size_t index = items_.size(); index > 0; --index) {
        if (std::holds_alternative<UserTurn>(items_[index - 1])) {
            prefix = index - 1;
            break;
        }
    }
    items_.emplace_back(
        CompactionEvent { id, CompactionEvent::Status::RUNNING });
    return { id, prefix };
}

void Session::finish_compaction(std::size_t id, std::string summary,
    std::size_t compacted_item_count, bool success)
{
    std::lock_guard lock(mutex_);
    for (auto& item : items_) {
        auto* event = std::get_if<CompactionEvent>(&item);
        if (event == nullptr || event->id != id) {
            continue;
        }
        event->status = success ? CompactionEvent::Status::COMPLETED
                                : CompactionEvent::Status::FAILED;
        if (success) {
            compacted_summary_    = std::move(summary);
            compacted_item_count_ = compacted_item_count;
        }
        return;
    }
}

void Session::append_tool(const ToolCallRequest& req)
{
    std::lock_guard lock(mutex_);
    if (auto* a = last_assistant_locked()) {
        finalize_reasoning(*a);
    }
    const std::size_t id = next_tool_id_++;
    items_.emplace_back(
        ToolCall { id, req.id, req.name, req.args, { }, { }, std::nullopt });
}

ToolCall* Session::_find_tool_locked(
    const ToolCallRequest& req, const bool unfinished_only)
{
    for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
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

void Session::set_tool_subagent_chats(
    const ToolCallRequest& req, std::vector<SubagentChat> chats)
{
    std::lock_guard lock(mutex_);
    if (auto* call = _find_tool_locked(req, false)) {
        call->subagent_chats = std::move(chats);
        ++content_serial_;
    }
}

void Session::set_tool_subagents(
    const ToolCallRequest& req, std::vector<std::size_t> ids)
{
    std::lock_guard lock(mutex_);
    if (auto* call = _find_tool_locked(req, true)) {
        call->subagent_ids = std::move(ids);
        ++content_serial_;
    }
}

void Session::fill_tool_result(
    const ToolCallRequest& req, ToolCall::Result result)
{
    std::lock_guard lock(mutex_);
    if (auto* call = _find_tool_locked(req, true)) {
        call->result = std::move(result);
    }
}

void Session::set_todo(TodoList todo)
{
    std::lock_guard lock(mutex_);
    todo_ = std::move(todo);
}

void Session::set_modal(ModalPayload payload)
{
    std::lock_guard lock(mutex_);
    modal_ = std::move(payload);
}

void Session::clear_modal()
{
    std::lock_guard lock(mutex_);
    modal_ = std::monostate { };
}

void Session::bump_modal_serial()
{
    std::lock_guard lock(mutex_);
    ++modal_serial_;
}

void Session::present_modal(ModalPayload payload)
{
    std::lock_guard lock(mutex_);
    modal_ = std::move(payload);
    if (std::holds_alternative<ToolCallRequest>(modal_)
        || std::holds_alternative<QuestionForm>(modal_)) {
        phase_ = Phase::AWAITING;
    }
    ++modal_serial_;
}

void Session::set_phase(Phase phase)
{
    std::lock_guard lock(mutex_);
    phase_ = phase;
}

void Session::mark_retry(int wait_seconds)
{
    std::lock_guard lock(mutex_);
    phase_           = Phase::CONNECTING;
    retry_countdown_ = Countdown { std::chrono::steady_clock::now()
        + std::chrono::seconds(wait_seconds) };
}

void Session::reset_reasoning()
{
    std::lock_guard lock(mutex_);
    reasoning_start_ = std::chrono::steady_clock::now();
}

void Session::request_interrupt() { interrupt_requested_.store(true); }

void Session::clear_interrupt() { interrupt_requested_.store(false); }

bool Session::interrupt_requested() const
{
    return interrupt_requested_.load();
}

std::optional<AssistantTurn> Session::last_assistant() const
{
    std::lock_guard lock(mutex_);
    if (const auto* assistant = last_assistant_locked()) {
        return *assistant;
    }
    return std::nullopt;
}

bool Session::finish_session(std::string error)
{
    std::lock_guard lock(mutex_);
    const bool finished = phase_ != Phase::IDLE;
    finish_session_locked(error);
    return finished;
}

std::vector<Message> Session::build_history(
    std::string_view system_prompt, ApiStandard dialect) const
{
    std::lock_guard lock(mutex_);
    std::vector<Message> history;
    history.push_back({ Message::Type::SYSTEM, std::string(system_prompt) });
    if (!compacted_summary_.empty()) {
        history.push_back({ Message::Type::USER,
            "<session-summary>\n" + compacted_summary_
                + "\n</session-summary>" });
    }
    const std::size_t begin = std::min(compacted_item_count_, items_.size());
    for (std::size_t index = begin; index < items_.size(); ++index) {
        const auto& item = items_[index];
        if (const auto* u = std::get_if<UserTurn>(&item)) {
            history.push_back({ Message::Type::USER,
                message_with_attachments(u->text, u->attachments) });
        } else if (const auto* a = std::get_if<AssistantTurn>(&item)) {
            Message m { Message::Type::ASSISTANT, a->markdown };
            if (dialect == ApiStandard::ANTHROPIC && !a->reasoning.empty()) {
                m.thinking.push_back({ a->reasoning, a->reasoning_signature });
            }
            history.push_back(std::move(m));
        } else if (const auto* tc = std::get_if<ToolCall>(&item)) {
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

    const ModeReminder injected = last_mode_reminder(history);
    if (mode_ == Mode::PLAN && injected != ModeReminder::PLAN) {
        for (Message& m : history) {
            if (m.type == Message::Type::USER) {
                m.content += "\n\n";
                m.content += plan_mode_reminder();
                break;
            }
        }
    } else if (mode_ == Mode::BUILD && injected == ModeReminder::PLAN) {
        for (auto it = history.rbegin(); it != history.rend(); ++it) {
            if (it->type == Message::Type::USER) {
                it->content += "\n\n";
                it->content += build_mode_reminder();
                break;
            }
        }
    }
    return history;
}

void Session::apply(const StreamEvent& ev, const ModelPricing& pricing)
{
    std::lock_guard lock(mutex_);
    switch (ev.kind) {
    case StreamEvent::Kind::CONTENT_DELTA:
        assert(phase_ != Phase::AWAITING);
        if (!items_.empty()) {
            if (auto* a = std::get_if<AssistantTurn>(&items_.back())) {
                if (!ev.text.empty()) {
                    finalize_reasoning(*a);
                }
                a->markdown += ev.text;
            }
        }
        break;
    case StreamEvent::Kind::REASONING:
        if (!items_.empty()) {
            if (auto* a = std::get_if<AssistantTurn>(&items_.back())) {
                if (a->reasoning.empty() && !reasoning_start_.has_value()) {
                    reasoning_start_ = std::chrono::steady_clock::now();
                }
                a->reasoning += ev.text;
                if (!ev.thinking_signature.empty()) {
                    a->reasoning_signature = ev.thinking_signature;
                }
            }
        }
        break;
    case StreamEvent::Kind::TOOL_CALL:
        if (auto* a = last_assistant_locked()) {
            finalize_reasoning(*a);
        }
        items_.emplace_back(ToolCall { next_tool_id_++, ev.tool_call.id,
            ev.tool_call.name, ev.tool_call.args, { }, { }, std::nullopt });
        break;
    case StreamEvent::Kind::QUESTION:
        if (!items_.empty()) {
            if (auto* a = std::get_if<AssistantTurn>(&items_.back())) {
                if (!a->markdown.empty()) {
                    a->markdown += "\n\n";
                }
                a->markdown += question_form_markdown(ev.question);
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
        if (phase_ == Phase::CONNECTING) {
            error_.clear();
            retry_countdown_.reset();
            phase_ = Phase::STREAMING;
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
    for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
        if (const auto* a = std::get_if<AssistantTurn>(&*it)) {
            return a;
        }
    }
    return nullptr;
}

void Session::finalize_reasoning(AssistantTurn& a)
{
    if (!reasoning_start_.has_value() || a.reasoning_ms.has_value()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    a.reasoning_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - *reasoning_start_);
}

void Session::finish_session_locked(const std::string& error)
{
    if (phase_ == Phase::IDLE) {
        return;
    }
    if (auto* a = last_assistant_locked()) {
        finalize_reasoning(*a);
    }
    retry_countdown_.reset();
    if (!error.empty() && error_.empty()) {
        error_ = error;
    }
    phase_ = Phase::IDLE;
}

void Session::update_usage(
    const StreamEvent& usage_event, const ModelPricing& pricing)
{
    last_ = usage_event.usage;
    totals_.prompt += usage_event.usage.prompt;
    totals_.completion += usage_event.usage.completion;
    totals_.total += usage_event.usage.total;
    total_cost_ += compute_cost(usage_event.usage, pricing);
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

} // namespace ursa
