#include "agent.h"
#include "format.h"
#include "prompt.h"
#include "util.h"

#include <cassert>

#include <cctype>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace ursa {

namespace {

    std::string denial_text(const std::string& reason)
    {
        if (reason.empty()) {
            return "user denied";
        }
        return "user denied: " + reason;
    }

    std::optional<QuestionForm> parse_ask_args(const std::string& args)
    {
        const Json::Value parsed = parse_json(args);
        if (!parsed.isObject() || !parsed["questions"].isArray()
            || parsed["questions"].empty()) {
            return std::nullopt;
        }
        QuestionForm form;
        for (const auto& q : parsed["questions"]) {
            QuestionCard card;
            if (!q.isObject() || !q["prompt"].isString()
                || q["prompt"].asString().empty()) {
                return std::nullopt;
            }
            card.prompt = q["prompt"].asString();
            if (q["options"].isArray()) {
                for (const auto& o : q["options"]) {
                    if (o.isString()) {
                        card.options.push_back(o.asString());
                    }
                }
            }
            if (q["multi"].isBool()) {
                card.multi = q["multi"].asBool();
            }
            if (q["free_text"].isBool()) {
                card.free_text = q["free_text"].asBool();
            }
            form.push_back(std::move(card));
        }
        if (form.empty()) {
            return std::nullopt;
        }
        return form;
    }

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

std::optional<TodoList> parse_todo_args(const Json::Value& args)
{
    if (!args.isObject() || !args["todos"].isArray()) {
        return std::nullopt;
    }
    TodoList list;
    for (const auto& entry : args["todos"]) {
        if (!entry.isObject() || !entry["content"].isString()
            || entry["content"].asString().empty()) {
            return std::nullopt;
        }
        TodoItem item;
        item.content = entry["content"].asString();
        if (entry["status"].isString()) {
            const std::string status = entry["status"].asString();
            if (status == "in_progress") {
                item.status = TodoItem::Status::IN_PROGRESS;
            } else if (status == "completed") {
                item.status = TodoItem::Status::COMPLETED;
            } else if (status == "cancelled") {
                item.status = TodoItem::Status::CANCELLED;
            } else if (status != "pending") {
                return std::nullopt;
            }
        }
        list.items.push_back(std::move(item));
    }
    return list;
}

std::string todo_summary(const TodoList& todo)
{
    static constexpr std::string_view marks[] = { "[ ]", "[→]", "[x]", "[-]" };
    std::string out;
    for (const auto& it : todo.items) {
        if (!out.empty()) {
            out += '\n';
        }
        out += marks[static_cast<std::size_t>(it.status)];
        out += ' ';
        out += it.content;
    }
    return out;
}

std::string error_text(Status st)
{
    return "stream error (" + std::to_string(static_cast<int>(st)) + ")";
}

Controller::Controller(const Config& cfg, PostFn post,
    std::function<void()> on_exit, StreamFn stream_fn, ToolRegistry tools,
    std::shared_future<Environment> env)
    : cfg_(cfg)
    , post_(std::move(post))
    , on_exit_(std::move(on_exit))
    , commands_(slash_commands(cfg))
    , stream_fn_(std::move(stream_fn))
    , tools_(std::move(tools))
    , env_(std::move(env))
{
    if (!stream_fn_) {
        stream_fn_ = [this](const ChatRequest& req, const StreamCallback& cb) {
            const auto provider = get_provider(cfg_);
            return stream(provider, cfg_, req, cb);
        };
    }
    if (env_.valid()) {
        env_waiter_ = std::jthread([this] {
            using namespace std::chrono_literals;
            while (env_.wait_for(0s) == std::future_status::timeout) {
                if (!alive_.load()) {
                    return;
                }
                _post([] { });
                std::this_thread::sleep_for(150ms);
            }
            if (!alive_.load()) {
                return;
            }
            _on_env_ready();
        });
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
    env_waiter_.reset();
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
    if (state_.modal.index() == 0
        && (state_.phase == UiState::Phase::IDLE
            || state_.phase == UiState::Phase::STREAMING)) {
        _present_front();
    }
}

void Controller::cancel_queued(std::size_t id)
{
    auto& q = state_.queued;
    for (auto it = q.begin(); it != q.end(); ++it) {
        if (it->id == id) {
            q.erase(it);
            return;
        }
    }
}

void Controller::_enqueue_message(std::string text)
{
    state_.queued.push_back(QueuedMessage { next_queued_id_++, std::move(text) });
}

void Controller::_drain_queued()
{
    if (state_.queued.empty()) {
        return;
    }
    QueuedMessage next = std::move(state_.queued.front());
    state_.queued.erase(state_.queued.begin());
    submit(std::move(next.text));
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
    const std::string_view t = trim(text);
    if (t.empty()) {
        return;
    }
    if (t[0] == '/') {
        run_slash(t);
        return;
    }
    if (state_.phase == UiState::Phase::IDLE) {
        submit_message(std::string(t));
    } else {
        _enqueue_message(std::string(t));
    }
}

void Controller::submit_message(std::string text)
{
    if (env_.valid() && !env_ready_.load()) {
        _enqueue_message(std::move(text));
        return;
    }
    state_.items.push_back(UserTurn { std::move(text) });
    state_.error.clear();
    state_.phase = UiState::Phase::STREAMING;
    _spawn(_build_history(), StreamFn { });
}

void Controller::_on_env_ready()
{
    env_ready_.store(true);
    _post([this] {
        state_.env_ready = true;
        _present_front();
        if (state_.phase == UiState::Phase::IDLE && !state_.queued.empty()) {
            _drain_queued();
        }
    });
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
            form.push_back(QuestionCard { "Which storage backend?",
                { "PostgreSQL", "SQLite", "MongoDB" }, false, false });
            form.push_back(QuestionCard { "Which features do you need?",
                { "Auth", "Billing", "Search", "Cache" }, true, false });
            form.push_back(QuestionCard { "Preferred cloud region?",
                { "us-east", "eu-west", "ap-south" }, false, true });
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
    case SlashCommand::Action::DEMO:
        if (state_.phase == UiState::Phase::IDLE) {
            run_demo();
        } else {
            _enqueue_message(std::string(cmd));
        }
        break;
    case SlashCommand::Action::SKILL:
        if (state_.phase == UiState::Phase::IDLE) {
            submit_message(std::string(cmd));
        } else {
            _enqueue_message(std::string(cmd));
        }
        break;
    case SlashCommand::Action::SYSTEM_PROMPT:
        enqueue_user_modal(SystemPromptModal { _system_prompt() });
        break;
    }
}

std::string Controller::shell_name() const
{
    if (env_.valid()
        && env_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const Environment& e = env_.get();
        if (!e.default_shell.empty()) {
            return std::filesystem::path(e.default_shell).filename().string();
        }
    }
    return "sh";
}

std::string Controller::_system_prompt() const
{
    const Environment* env = nullptr;
    if (env_.valid()
        && env_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        env = &env_.get();
    }
    return build_system_prompt(env);
}

std::vector<Message> Controller::_build_history() const
{
    std::vector<Message> history;
    history.push_back({ Message::Type::SYSTEM, _system_prompt() });
    for (const auto& item : state_.items) {
        if (const auto* u = std::get_if<UserTurn>(&item)) {
            history.push_back({ Message::Type::USER, u->text });
        } else if (const auto* a = std::get_if<AssistantTurn>(&item)) {
            history.push_back({ Message::Type::ASSISTANT, a->markdown });
        } else if (const auto* tc = std::get_if<ToolCall>(&item)) {
            if (history.empty()
                || history.back().type != Message::Type::ASSISTANT) {
                history.push_back({ Message::Type::ASSISTANT, "" });
            }
            history.back().tool_calls.push_back(
                ToolCallEntry { tc->call_id, tc->name, tc->args });
            history.push_back({ Message::Type::TOOL, _tool_result_text(*tc),
                { }, tc->call_id });
        }
    }

    const ModeReminder injected = last_mode_reminder(history);
    if (state_.mode == UiState::Mode::PLAN
        && injected != ModeReminder::PLAN) {
        for (Message& m : history) {
            if (m.type == Message::Type::USER) {
                m.content += "\n\n";
                m.content += plan_mode_reminder();
                break;
            }
        }
    } else if (state_.mode == UiState::Mode::BUILD
        && injected == ModeReminder::PLAN) {
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
        req.tools    = state_.mode == UiState::Mode::PLAN
            ? tools_.specs(ToolSafety::READ_ONLY)
            : tools_.specs();
        stream_events_.clear();
        std::string text_buffer;

        StreamFn fn     = override ? override : stream_fn_;
        const Status st = fn(req, [this, &text_buffer](const StreamEvent& ev) {
            if (ev.kind == StreamEvent::Kind::TOOL_CALL
                || ev.kind == StreamEvent::Kind::QUESTION) {
                stream_events_.push_back(ev);
            }
            if (ev.kind == StreamEvent::Kind::CONTENT_DELTA) {
                text_buffer += ev.text;
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
        _drain_pending_asks(history, reply_buffer, text_buffer);
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

void Controller::_drain_pending_asks(std::vector<Message>& history,
    std::string& reply_buffer, const std::string& assistant_text)
{
    struct Ask {
        ModalPayload payload;
        std::future<ModalResult> future;
        std::optional<ToolCallRequest> tool_req;
    };
    std::vector<Ask> asks;
    std::vector<Message> tool_msgs;
    bool had_tool_calls = false;

    for (const auto& ev : stream_events_) {
        if (ev.kind == StreamEvent::Kind::TOOL_CALL) {
            had_tool_calls = true;
            if (ev.tool_call.name == "ask") {
                auto form = parse_ask_args(ev.tool_call.args);
                if (!form) {
                    const ToolCallRequest req = ev.tool_call;
                    _post([this, req] {
                        _fill_tool_result(req,
                            ToolCall::Result { ToolCall::Result::Kind::ERROR,
                                "ask: expected a non-empty 'questions' array" });
                    });
                    tool_msgs.push_back(
                        { Message::Type::TOOL,
                            "ask: expected a non-empty 'questions' array", { },
                            req.id });
                    continue;
                }
                auto promise = std::make_shared<std::promise<ModalResult>>();
                auto future  = promise->get_future();
                {
                    std::lock_guard lock(queue_mutex_);
                    queue_.push_back(
                        PendingModal { *form, std::move(promise) });
                }
                Ask ask;
                ask.payload   = *form;
                ask.future    = std::move(future);
                ask.tool_req  = ev.tool_call;
                asks.push_back(std::move(ask));
                continue;
            }
            if (ev.tool_call.name == "todo") {
                const std::optional<TodoList> list
                    = parse_todo_args(parse_json(ev.tool_call.args));
                const ToolCallRequest req = ev.tool_call;
                if (!list.has_value()) {
                    const std::string msg
                        = "todo: expected a 'todos' array of {content, status} "
                          "objects";
                    _post([this, req, msg] {
                        _fill_tool_result(req,
                            ToolCall::Result { ToolCall::Result::Kind::ERROR,
                                msg });
                    });
                    tool_msgs.push_back(
                        { Message::Type::TOOL, msg, { }, req.id });
                    continue;
                }
                const std::string text = todo_summary(*list);
                _post([this, req, todo = *list, text] {
                    state_.todo = todo;
                    _fill_tool_result(req,
                        ToolCall::Result { ToolCall::Result::Kind::OUTPUT,
                            text });
                });
                tool_msgs.push_back({ Message::Type::TOOL, text, { }, req.id });
                continue;
            }
            const Tool* tool = tools_.find(ev.tool_call.name);
            const bool needs_approval = tool != nullptr
                && tool->safety == ToolSafety::MUTATING
                && allowed_tools_.count(ev.tool_call.name) == 0;
            if (!needs_approval) {
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
            asks.push_back(Ask { .payload = ev.tool_call,
                .future  = std::move(future), .tool_req = std::nullopt });
        } else if (ev.kind == StreamEvent::Kind::QUESTION) {
            auto promise = std::make_shared<std::promise<ModalResult>>();
            auto future  = promise->get_future();
            {
                std::lock_guard lock(queue_mutex_);
                queue_.push_back(
                    PendingModal { ev.question, std::move(promise) });
            }
            asks.push_back(Ask { .payload = ev.question,
                .future  = std::move(future), .tool_req = std::nullopt });
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
            _apply_tool_result(*req, res, tool_msgs);
        } else if (ask.tool_req.has_value()) {
            _apply_ask_result(*ask.tool_req, res, tool_msgs);
        } else {
            _apply_question_result(res, reply_buffer);
        }
    }

    if (!had_tool_calls && asks.empty()) {
        return;
    }

    Message assistant { Message::Type::ASSISTANT, assistant_text };
    for (const auto& ev : stream_events_) {
        if (ev.kind == StreamEvent::Kind::TOOL_CALL) {
            assistant.tool_calls.push_back(ToolCallEntry { ev.tool_call.id,
                ev.tool_call.name, ev.tool_call.args });
        }
    }
    if (!assistant.tool_calls.empty() || !assistant.content.empty()) {
        history.push_back(std::move(assistant));
    }
    for (auto& m : tool_msgs) {
        history.push_back(std::move(m));
    }
    if (!reply_buffer.empty()) {
        history.push_back({ Message::Type::USER, reply_buffer });
    }
}

void Controller::_apply_tool_result(const ToolCallRequest& req,
    const ModalResult& res, std::vector<Message>& tool_msgs)
{
    const auto* verdict = std::get_if<ToolVerdict>(&res);
    if (verdict == nullptr) {
        _post([this, req] {
            _fill_tool_result(
                req, ToolCall::Result { ToolCall::Result::Kind::CANCEL, "" });
        });
        tool_msgs.push_back(
            { Message::Type::TOOL, denial_text(""), { }, req.id });
        return;
    }
    if (verdict->decision == ToolDecision::REJECT) {
        std::string reason = verdict->reason;
        _post([this, req, reason] {
            _fill_tool_result(req,
                ToolCall::Result { ToolCall::Result::Kind::REJECT, reason });
        });
        tool_msgs.push_back(
            { Message::Type::TOOL, denial_text(reason), { }, req.id });
        return;
    }
    if (verdict->decision == ToolDecision::ACCEPT_ALWAYS) {
        const Tool* tool = tools_.find(req.name);
        if (tool == nullptr || tool->persistent) {
            allowed_tools_.insert(req.name);
        }
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

void Controller::_apply_ask_result(const ToolCallRequest& req,
    const ModalResult& res, std::vector<Message>& tool_msgs)
{
    const auto* answer = std::get_if<ModalAnswer>(&res);
    if (answer == nullptr) {
        _post([this, req] {
            _fill_tool_result(req,
                ToolCall::Result { ToolCall::Result::Kind::CANCEL, "" });
        });
        tool_msgs.push_back(
            { Message::Type::TOOL, denial_text(""), { }, req.id });
        return;
    }
    ModalAnswer copy = *answer;
    const std::string text = ask_answer_markdown(copy);
    _post([this, req, text] {
        _fill_tool_result(req,
            ToolCall::Result { ToolCall::Result::Kind::OUTPUT, text });
    });
    tool_msgs.push_back({ Message::Type::TOOL, text, { }, req.id });
}

void Controller::_run_tool(
    const ToolCallRequest& req, std::vector<Message>& tool_msgs)
{
    const ToolOutput out = tools_.dispatch(req);
    const auto kind      = out.kind == ToolOutput::Kind::OUTPUT
              ? ToolCall::Result::Kind::OUTPUT
              : ToolCall::Result::Kind::ERROR;
    _post([this, req, kind, out] {
        _fill_tool_result(req, ToolCall::Result { kind, out.text });
    });
    tool_msgs.push_back({ Message::Type::TOOL, out.text, { }, req.id });
}

std::string Controller::_tool_result_text(const ToolCall& call)
{
    if (!call.result.has_value()) {
        return "";
    }
    switch (call.result->kind) {
    case ToolCall::Result::Kind::REJECT:
    case ToolCall::Result::Kind::CANCEL:
        return denial_text(call.result->text);
    case ToolCall::Result::Kind::OUTPUT:
    case ToolCall::Result::Kind::ERROR: return call.result->text;
    }
    return "";
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
        state_.items.push_back(ToolCall { next_tool_id_++, ev.tool_call.id,
            ev.tool_call.name, ev.tool_call.args, std::nullopt });
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
    _drain_queued();
}

std::vector<SlashCommand> slash_commands(const Config&)
{
    return {
        { "/help", "show available commands", SlashCommand::Action::HELP },
        { "/exit", "quit ursa", SlashCommand::Action::EXIT },
        { "/settings", "open settings", SlashCommand::Action::SETTINGS },
        { "/demo", "run scripted modal demo", SlashCommand::Action::DEMO },
        { "/prompt", "show the generated system prompt",
            SlashCommand::Action::SYSTEM_PROMPT },
    };
}

const SlashCommand* find_command(
    const std::vector<SlashCommand>& commands, std::string_view name)
{
    const std::string key = to_lower(name);
    for (const auto& c : commands) {
        if (to_lower(c.name) == key) {
            return &c;
        }
    }
    return nullptr;
}

} // namespace ursa
