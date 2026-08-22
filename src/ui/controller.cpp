#include "render.h"
#include "ui.h"

#include <cassert>
#include <thread>

#include <algorithm>
#include <cctype>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace ursa {

namespace {

    std::string_view trim(std::string_view s)
    {
        size_t b = 0;
        size_t e = s.size();
        while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
            ++b;
        }
        while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
            --e;
        }
        return s.substr(b, e - b);
    }

    std::string denial_text(const std::string& reason)
    {
        if (reason.empty()) {
            return "user denied";
        }
        return "user denied: " + reason;
    }

} // namespace

std::string error_text(Status st)
{
    return "stream error (" + std::to_string(static_cast<int>(st)) + ")";
}

std::string Controller::default_tool_output(const ToolCallRequest& req)
{
    return "[" + req.name + " stub output]";
}

Controller::Controller(const Config& cfg, PostFn post,
    std::function<void()> on_exit, StreamFn stream_fn, ToolRunner tool_runner)
    : cfg_(cfg)
    , post_(std::move(post))
    , on_exit_(std::move(on_exit))
    , commands_(slash_commands(cfg))
    , stream_fn_(std::move(stream_fn))
    , tool_runner_(std::move(tool_runner))
{
    if (!stream_fn_) {
        stream_fn_ = [this](const ChatRequest& req, const StreamCallback& cb) {
            const auto provider = get_provider(cfg_);
            return stream(provider, cfg_, req, cb);
        };
    }
    if (!tool_runner_) {
        tool_runner_ = [](const ToolCallRequest& req) {
            return default_tool_output(req);
        };
    }
}

Controller::~Controller()
{
    alive_.store(false);
    {
        std::lock_guard lock(queue_mutex_);
        for (auto& entry : queue_) {
            if (entry.promise) {
                entry.promise->set_value(std::monostate { });
            }
        }
    }
    worker_.reset();
}

void Controller::toggle_mode()
{
    state_.mode = (state_.mode == UiState::Mode::PLAN) ? UiState::Mode::BUILD
                                                       : UiState::Mode::PLAN;
}

void Controller::set_error(std::string msg) { state_.error = std::move(msg); }

void Controller::close_modal() { resolve_modal(std::monostate { }); }

void Controller::enqueue_user_modal(ModalPayload payload)
{
    {
        std::lock_guard lock(queue_mutex_);
        queue_.push_back(PendingModal { std::move(payload),
            std::shared_ptr<std::promise<ModalResult>> { } });
    }
    if (state_.phase == UiState::Phase::IDLE && state_.modal.index() == 0) {
        _present_front();
    }
}

size_t Controller::queue_size() const
{
    std::lock_guard lock(queue_mutex_);
    return queue_.size();
}

ModalResult Controller::request_modal(ModalPayload payload)
{
    if (!alive_.load()) {
        return std::monostate { };
    }
    auto promise = std::make_shared<std::promise<ModalResult>>();
    std::future<ModalResult> future = promise->get_future();
    {
        std::lock_guard lock(queue_mutex_);
        queue_.push_back(
            PendingModal { std::move(payload), std::move(promise) });
    }
    _post([this] { _present_front(); });
    return future.get();
}

void Controller::resolve_modal(ModalResult result)
{
    {
        std::lock_guard lock(queue_mutex_);
        if (!queue_.empty()) {
            auto entry = queue_.front();
            queue_.pop_front();
            if (entry.promise) {
                entry.promise->set_value(std::move(result));
            }
        }
    }
    state_.modal = std::monostate { };
    if (state_.phase == UiState::Phase::AWAITING) {
        state_.phase = UiState::Phase::STREAMING;
    }
    _present_front();
}

void Controller::_present_front()
{
    std::lock_guard lock(queue_mutex_);
    if (state_.modal.index() != 0 || queue_.empty()) {
        return;
    }
    state_.modal = queue_.front().payload;
    if (std::holds_alternative<ToolCallRequest>(state_.modal)
        || std::holds_alternative<QuestionForm>(state_.modal)) {
        state_.phase = UiState::Phase::AWAITING;
    }
    ++state_.modal_serial;
}

void Controller::submit(std::string text)
{
    if (state_.phase != UiState::Phase::IDLE) {
        return;
    }
    const std::string_view t = trim(text);
    if (t.empty()) {
        return;
    }
    if (t[0] == '/') {
        run_slash(t);
        return;
    }
    submit_message(std::string(t));
}

void Controller::submit_message(std::string text)
{
    state_.items.push_back(UserTurn { std::move(text) });
    state_.error.clear();
    state_.phase = UiState::Phase::STREAMING;
    _spawn(_build_history(), StreamFn { });
}

void Controller::run_demo()
{
    if (state_.phase != UiState::Phase::IDLE) {
        return;
    }
    state_.items.push_back(UserTurn { "/demo" });
    state_.error.clear();
    state_.phase = UiState::Phase::STREAMING;

    auto round = std::make_shared<int>(0);
    StreamFn script
        = [round](const ChatRequest&, const StreamCallback& cb) -> Status {
        switch (*round) {
        case 0: {
            cb(make_delta_event("Let me gather some details first.\n\n"));
            QuestionForm form;
            form.push_back(QuestionCard { "Which storage backend should I use?",
                { "PostgreSQL", "SQLite", "MongoDB" }, false, false });
            form.push_back(QuestionCard {
                "Anything else I should know?", { }, false, true });
            cb(make_question_event(std::move(form)));
            break;
        }
        case 1: {
            cb(make_delta_event("Setting things up.\n\n"));
            cb(make_tool_call_event(ToolCallRequest {
                "bash", "ls -la", "list working directory" }));
            break;
        }
        default: cb(make_delta_event("Done — demo complete.")); break;
        }
        ++*round;
        cb(make_done_event());
        return Status::OK;
    };

    _spawn(_build_history(), std::move(script));
}

void Controller::run_slash(std::string_view cmd)
{
    const SlashCommand* found = find_command(commands_, cmd);
    if (found == nullptr) {
        set_error("unknown command: " + std::string(cmd));
        return;
    }
    switch (found->action) {
    case SlashCommand::Action::EXIT: on_exit_(); break;
    case SlashCommand::Action::HELP: enqueue_user_modal(HelpModal { }); break;
    case SlashCommand::Action::SETTINGS:
        enqueue_user_modal(SettingsModal { cfg_.model });
        break;
    case SlashCommand::Action::DEMO: run_demo(); break;
    case SlashCommand::Action::SKILL: submit_message(std::string(cmd)); break;
    }
}

std::vector<Message> Controller::_build_history() const
{
    std::vector<Message> history;
    history.push_back(
        { Message::Type::SYSTEM, "You are a helpful assistant." });
    for (const auto& item : state_.items) {
        if (const auto* u = std::get_if<UserTurn>(&item)) {
            history.push_back({ Message::Type::USER, u->text });
        } else if (const auto* a = std::get_if<AssistantTurn>(&item)) {
            history.push_back({ Message::Type::ASSISTANT, a->markdown });
        }
    }
    return history;
}

void Controller::_post(std::function<void()> f)
{
    if (alive_.load()) {
        post_(std::move(f));
    }
}

void Controller::_spawn(std::vector<Message> history, StreamFn override)
{
    state_.items.push_back(AssistantTurn { });
    worker_.emplace([this, history = std::move(history),
                        override = std::move(override)]() mutable {
        _drive(std::move(history), std::move(override));
    });
}

void Controller::_drive(std::vector<Message> history, StreamFn override)
{
    for (;;) {
        ChatRequest req;
        req.model    = cfg_.model;
        req.messages = history;
        stream_events_.clear();

        StreamFn fn     = override ? override : stream_fn_;
        const Status st = fn(req, [this](const StreamEvent& ev) {
            if (ev.kind == StreamEvent::Kind::TOOL_CALL
                || ev.kind == StreamEvent::Kind::QUESTION) {
                stream_events_.push_back(ev);
            }
            _post([this, ev] { apply(ev); });
        });
        if (st != Status::OK) {
            _post([this, st] { finish(error_text(st)); });
            return;
        }
        if (!alive_.load()) {
            return;
        }

        std::string reply_buffer;
        const size_t history_before = history.size();
        _drain_pending_asks(history, reply_buffer);
        if (!alive_.load()) {
            return;
        }

        if (history.size() == history_before) {
            _post([this] { finish(""); });
            return;
        }
        _post([this] { state_.items.push_back(AssistantTurn { }); });
    }
}

void Controller::_drain_pending_asks(
    std::vector<Message>& history, std::string& reply_buffer)
{
    struct Ask {
        ModalPayload payload;
        std::future<ModalResult> future;
    };
    std::vector<Ask> asks;
    std::vector<Message> tool_msgs;

    for (const auto& ev : stream_events_) {
        if (ev.kind == StreamEvent::Kind::TOOL_CALL) {
            if (allowed_tools_.count(ev.tool_call.name) != 0) {
                _run_tool(ev.tool_call, tool_msgs);
                continue;
            }
            auto promise = std::make_shared<std::promise<ModalResult>>();
            auto future  = promise->get_future();
            {
                std::lock_guard lock(queue_mutex_);
                queue_.push_back(
                    PendingModal { ev.tool_call, std::move(promise) });
            }
            asks.push_back(Ask { ev.tool_call, std::move(future) });
        } else if (ev.kind == StreamEvent::Kind::QUESTION) {
            auto promise = std::make_shared<std::promise<ModalResult>>();
            auto future  = promise->get_future();
            {
                std::lock_guard lock(queue_mutex_);
                queue_.push_back(
                    PendingModal { ev.question, std::move(promise) });
            }
            asks.push_back(Ask { ev.question, std::move(future) });
        }
    }
    if (!asks.empty()) {
        _post([this] { _present_front(); });
    }

    for (auto& ask : asks) {
        if (!alive_.load()) {
            return;
        }
        const ModalResult res = ask.future.get();
        if (const auto* req = std::get_if<ToolCallRequest>(&ask.payload)) {
            _apply_tool_result(*req, res, tool_msgs, reply_buffer);
        } else {
            _apply_question_result(res, reply_buffer);
        }
    }

    if (!reply_buffer.empty()) {
        history.push_back({ Message::Type::USER, reply_buffer });
    }
    for (auto& m : tool_msgs) {
        history.push_back(std::move(m));
    }
}

void Controller::_apply_tool_result(const ToolCallRequest& req,
    const ModalResult& res, std::vector<Message>& tool_msgs,
    std::string& reply_buffer)
{
    const auto* verdict = std::get_if<ToolVerdict>(&res);
    if (verdict == nullptr) {
        _post([this, req] {
            _fill_tool_result(
                req, ToolCall::Result { ToolCall::Result::Kind::CANCEL, "" });
        });
        reply_buffer += tool_request_markdown(req) + "\n---\n" + denial_text("")
            + "\n\n";
        return;
    }
    if (verdict->decision == ToolDecision::REJECT) {
        std::string reason = verdict->reason;
        _post([this, req, reason] {
            _fill_tool_result(req,
                ToolCall::Result { ToolCall::Result::Kind::REJECT, reason });
        });
        reply_buffer += tool_request_markdown(req) + "\n---\n"
            + denial_text(reason) + "\n\n";
        return;
    }
    if (verdict->decision == ToolDecision::ACCEPT_ALWAYS) {
        allowed_tools_.insert(req.name);
    }
    _run_tool(req, tool_msgs);
}

void Controller::_apply_question_result(
    const ModalResult& res, std::string& reply_buffer)
{
    const auto* answer = std::get_if<ModalAnswer>(&res);
    if (answer == nullptr) {
        return;
    }
    ModalAnswer copy = *answer;
    reply_buffer += modal_answer_markdown(copy);
    _post([this, copy] { state_.items.push_back(std::move(copy)); });
}

void Controller::_run_tool(
    const ToolCallRequest& req, std::vector<Message>& tool_msgs)
{
    const std::string out = tool_runner_(req);
    _post([this, req, out] {
        _fill_tool_result(
            req, ToolCall::Result { ToolCall::Result::Kind::OUTPUT, out });
    });
    tool_msgs.push_back(
        { Message::Type::USER, tool_request_markdown(req) + "\n---\n" + out });
}

void Controller::_fill_tool_result(
    const ToolCallRequest& req, ToolCall::Result result)
{
    for (auto it = state_.items.rbegin(); it != state_.items.rend(); ++it) {
        auto* tc = std::get_if<ToolCall>(&*it);
        if (tc == nullptr || tc->result.has_value()) {
            continue;
        }
        if (tc->name == req.name && tc->args == req.args) {
            tc->result = std::move(result);
            return;
        }
    }
}

void Controller::apply(const StreamEvent& ev)
{
    switch (ev.kind) {
    case StreamEvent::Kind::CONTENT_DELTA:
        assert(state_.phase != UiState::Phase::AWAITING);
        if (!state_.items.empty()) {
            if (auto* a = std::get_if<AssistantTurn>(&state_.items.back())) {
                a->markdown += ev.text;
            }
        }
        break;
    case StreamEvent::Kind::TOOL_CALL:
        state_.items.push_back(ToolCall { next_tool_id_++, ev.tool_call.name,
            ev.tool_call.args, std::nullopt });
        break;
    case StreamEvent::Kind::QUESTION:
        if (!state_.items.empty()) {
            if (auto* a = std::get_if<AssistantTurn>(&state_.items.back())) {
                if (!a->markdown.empty()) {
                    a->markdown += "\n\n";
                }
                a->markdown += question_form_markdown(ev.question);
            }
        }
        break;
    case StreamEvent::Kind::DONE: break;
    case StreamEvent::Kind::ERROR: finish(error_text(ev.error)); break;
    }
}

void Controller::finish(std::string error)
{
    if (state_.phase == UiState::Phase::IDLE) {
        return;
    }
    if (!error.empty()) {
        state_.error = std::move(error);
    }
    state_.phase = UiState::Phase::IDLE;
    _present_front();
}

} // namespace ursa
