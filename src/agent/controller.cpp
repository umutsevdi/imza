#include "agent.h"
#include "format.h"
#include "pricing.h"
#include "prompt.h"
#include "util.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <thread>

#include <cctype>
#include <filesystem>
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

    bool auto_approved_in_project(const std::string& name,
        const std::string& args)
    {
        if (name != "edit" && name != "write") {
            return false;
        }
        const Json::Value parsed = parse_json(args);
        if (!parsed.isObject() || !parsed["file_path"].isString()) {
            return false;
        }
        std::error_code ec;
        const std::filesystem::path target = std::filesystem::weakly_canonical(
            std::filesystem::absolute(parsed["file_path"].asString()), ec);
        if (ec) {
            return false;
        }
        const std::filesystem::path root = std::filesystem::weakly_canonical(
            std::filesystem::current_path(ec), ec);
        if (ec) {
            return false;
        }
        const auto rel = target.lexically_relative(root);
        if (rel.empty()) {
            return true;
        }
        const std::string rel_str = rel.string();
        return rel_str.rfind("..", 0) != 0;
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
    switch (st) {
    case Status::OK: return "";
    case Status::NETWORK_ERROR: return "network error";
    case Status::INVALID_URL: return "invalid API URL";
    case Status::JSON_ERROR: return "malformed response from provider";
    case Status::API_ERROR: return "API error";
    case Status::RATE_LIMITED: return "rate limited by provider";
    case Status::BUDGET_EXCEEDED: return "out of budget / insufficient credits";
    case Status::UNSUPPORTED: return "unsupported operation";
    case Status::CONFIG_ERROR: return "configuration error";
    }
    return "unknown error";
}

Controller::Controller(const Config& cfg, PostFn post,
    std::function<void()> on_exit, StreamFn stream_fn, ToolRegistry tools,
    std::shared_future<Environment> env, ModelsFn models_fn)
    : cfg_(cfg)
    , post_(std::move(post))
    , on_exit_(std::move(on_exit))
    , commands_(slash_commands())
    , stream_fn_(std::move(stream_fn))
    , tools_(std::move(tools))
    , env_(std::move(env))
    , models_fn_(std::move(models_fn))
{
    specs_plan_ = tools_.specs(ToolSafety::READ_ONLY);
    specs_all_  = tools_.specs();

    load_catalog(presets_path(), catalog_);
    set_pricing_catalog(catalog_);

    if (!stream_fn_) {
        stream_fn_ = [this](const ChatRequest& req, const StreamCallback& cb) {
            Route route;
            {
                std::lock_guard lock(data_mutex_);
                route = _active_route_locked(req.model);
            }
            return stream(
                get_provider(route), route, req, cb, &retry_after_secs_);
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
    _post([this] {
        std::lock_guard lock(data_mutex_);
        for (const Connection& conn : cfg_.providers) {
            _start_fetch_locked(conn.id);
        }
    });
}

void Controller::ensure_catalog_fresh()
{
    std::lock_guard lock(data_mutex_);
    if (catalog_syncing_ || !catalog_stale(catalog_)) {
        return;
    }
    catalog_syncing_ = true;
    auto future      = std::async(std::launch::async, [] {
        Catalog catalog;
        const Status st = fetch_catalog(catalog);
        return std::make_pair(st, catalog);
    }).share();
    catalog_waiter_.emplace([this, future] {
        future.wait();
        if (!alive_.load()) {
            return;
        }
        _post([this, future] {
            auto [st, catalog] = future.get();
            {
                std::lock_guard lock(data_mutex_);
                catalog_syncing_ = false;
                if (st != Status::OK) {
                    return;
                }
                catalog_ = catalog;
                save_catalog(presets_path(), catalog_);
                set_pricing_catalog(catalog_);
            }
            ++state_.modal_serial;
        });
    });
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
    catalog_waiter_.reset();
    fetch_threads_.clear();
}

void Controller::toggle_mode()
{
    state_.mode = (state_.mode == UiState::Mode::PLAN) ? UiState::Mode::BUILD
                                                       : UiState::Mode::PLAN;
}

void Controller::set_error(std::string msg) { state_.error = std::move(msg); }

void Controller::clear_error() { state_.error.clear(); }

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
            || state_.phase == UiState::Phase::CONNECTING
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
    state_.queued.push_back(
        QueuedMessage { next_queued_id_++, std::move(text) });
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
    if (auto* connect = std::get_if<ConnectResult>(&result)) {
        _begin_connect(*connect);
        return;
    }
    if (auto* choice = std::get_if<ModelChoice>(&result)) {
        _apply_pick(*choice);
    }
    if (auto* variant = std::get_if<VariantChoice>(&result)) {
        {
            std::lock_guard lock(data_mutex_);
            cfg_.reasoning_effort = variant->effort;
            save_config(config_path(), cfg_);
        }
        ++state_.modal_serial;
    }
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
        state_.phase = UiState::Phase::CONNECTING;
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
    {
        std::lock_guard lock(data_mutex_);
        if (!cfg_.last_used || cfg_.last_used->model.empty()) {
            state_.error = "no model selected — run /model";
            return;
        }
    }
    if (env_.valid() && !env_ready_.load()) {
        _enqueue_message(std::move(text));
        return;
    }
    state_.items.push_back(UserTurn { std::move(text) });
    state_.error.clear();
    state_.phase = UiState::Phase::CONNECTING;
    _spawn(_build_history(), StreamFn { });
}

void Controller::_on_env_ready()
{
    env_ready_.store(true);
    _post([this] {
        state_.env_ready = true;
        if (env_.valid() && env_.get().instruction) {
            state_.agent_rules    = env_.get().instruction->path;
            state_.project_skills = env_.get().project_skills.size();
            state_.global_skills  = env_.get().global_skills.size();
        }
        _present_front();
        if (state_.phase == UiState::Phase::IDLE && !state_.queued.empty()) {
            _drain_queued();
        }
    });
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
    case SlashCommand::Action::CONNECT:
        if (state_.phase == UiState::Phase::IDLE) {
            enqueue_user_modal(ConnectModal { ConnectModal::Entry::MANAGE });
        } else {
            _enqueue_message(std::string(cmd));
        }
        break;
    case SlashCommand::Action::MODEL: {
        if (state_.phase != UiState::Phase::IDLE) {
            _enqueue_message(std::string(cmd));
            break;
        }
        bool any = false;
        {
            std::lock_guard lock(data_mutex_);
            any = !cfg_.providers.empty();
        }
        if (!any) {
            set_error("no connections — run /connect first");
            break;
        }
        enqueue_user_modal(ConnectModal { ConnectModal::Entry::PICK_MODEL });
        break;
    }
    case SlashCommand::Action::VARIANT: {
        if (state_.phase != UiState::Phase::IDLE) {
            _enqueue_message(std::string(cmd));
            break;
        }
        std::string current;
        {
            std::lock_guard lock(data_mutex_);
            current = cfg_.reasoning_effort.value_or("default");
            if (current == "medium") {
                current = "default";
            }
        }
        enqueue_user_modal(VariantModal {
            { "off", "low", "default", "high" }, current });
        break;
    }
    case SlashCommand::Action::SYSTEM_PROMPT:
        enqueue_user_modal(ViewerModal {
            "System prompt", _system_prompt(), "text", 1, false });
        break;
    }
}

std::string Controller::shell_name() const
{
    if (env_.valid()
        && env_.wait_for(std::chrono::seconds(0))
            == std::future_status::ready) {
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
        && env_.wait_for(std::chrono::seconds(0))
            == std::future_status::ready) {
        env = &env_.get();
    }
    return build_system_prompt(env);
}

bool Controller::_model_reasons(const std::string& model) const
{
    if (model.empty()) {
        return false;
    }
    for (const auto& [provider_id, provider] : catalog_.providers) {
        auto it = provider.models.find(model);
        if (it != provider.models.end() && it->second.reasoning == true) {
            return true;
        }
    }
    return false;
}

std::uint64_t Controller::_budget_for_effort(const std::string& effort) const
{
    if (effort == "low") {
        return 2000;
    }
    if (effort == "high") {
        return 16000;
    }
    return 8000;
}

void Controller::_set_reasoning(ChatRequest& req, ApiStandard dialect)
{
    req.reasoning_effort.reset();
    req.thinking_budget.reset();
    std::string effort = cfg_.reasoning_effort.value_or("default");
    if (effort == "medium") {
        effort = "default";
    }
    if (effort == "off" || !_model_reasons(req.model)) {
        return;
    }
    if (dialect == ApiStandard::ANTHROPIC) {
        req.thinking_budget = _budget_for_effort(effort);
    } else {
        req.reasoning_effort = effort == "default" ? "medium" : effort;
    }
}

AssistantTurn* Controller::_last_assistant()
{
    for (auto it = state_.items.rbegin(); it != state_.items.rend(); ++it) {
        if (auto* a = std::get_if<AssistantTurn>(&*it)) {
            return a;
        }
    }
    return nullptr;
}

void Controller::_finalize_reasoning(AssistantTurn& a)
{
    if (!reasoning_start_.has_value() || a.reasoning.empty()
        || a.reasoning_ms.has_value()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    a.reasoning_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - *reasoning_start_);
}

std::vector<Message> Controller::_build_history(ApiStandard dialect) const
{
    std::vector<Message> history;
    history.push_back({ Message::Type::SYSTEM, _system_prompt() });
    for (const auto& item : state_.items) {
        if (const auto* u = std::get_if<UserTurn>(&item)) {
            history.push_back({ Message::Type::USER, u->text });
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
            history.push_back({ Message::Type::TOOL, _tool_result_text(*tc),
                { }, tc->call_id });
        }
    }

    const ModeReminder injected = last_mode_reminder(history);
    if (state_.mode == UiState::Mode::PLAN && injected != ModeReminder::PLAN) {
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
    int retries = 0;
    for (;;) {
        ChatRequest req;
        {
            std::lock_guard lock(data_mutex_);
            req.model = cfg_.last_used ? cfg_.last_used->model : "";
        }
        req.messages = history;
        req.tools
            = state_.mode == UiState::Mode::PLAN ? specs_plan_ : specs_all_;
        reasoning_start_.reset();
        stream_events_.clear();
        std::string text_buffer;
        std::string error_msg;
        Status error_status = Status::OK;
        bool saw_stream     = false;

        StreamCallback cb = [this, &text_buffer, &error_msg, &error_status,
                                &saw_stream](const StreamEvent& ev) {
            if (ev.kind == StreamEvent::Kind::ERROR) {
                error_status = ev.error;
                error_msg    = ev.text;
                return;
            }
            if (ev.kind == StreamEvent::Kind::TOOL_CALL
                || ev.kind == StreamEvent::Kind::QUESTION) {
                stream_events_.push_back(ev);
            }
            if (ev.kind == StreamEvent::Kind::CONTENT_DELTA
                || ev.kind == StreamEvent::Kind::CONNECTED) {
                saw_stream = true;
            }
            if (ev.kind == StreamEvent::Kind::CONTENT_DELTA) {
                text_buffer += ev.text;
            }
            _post([this, ev] { apply(ev); });
        };

        StreamFn fn       = override ? override : stream_fn_;
        retry_after_secs_ = 0;
        Status st;
        ApiStandard active_dialect = ApiStandard::OPENAI;
        if (override) {
            st = fn(req, cb);
        } else {
            Route route;
            {
                std::lock_guard lock(data_mutex_);
                route = _active_route_locked(req.model);
                _set_reasoning(req, route.dialect);
                active_dialect = route.dialect;
            }
            st = stream(
                get_provider(route), route, req, cb, &retry_after_secs_);
            const Status attempt
                = error_status != Status::OK ? error_status : st;
            if (attempt == Status::API_ERROR && !saw_stream
                && route.dialect == ApiStandard::OPENAI) {
                Route alt;
                bool has_alt = false;
                {
                    std::lock_guard lock(data_mutex_);
                    if (cfg_.last_used) {
                        if (Connection* conn
                            = _find_locked(cfg_.last_used->provider)) {
                            alt = resolve_route(
                                *conn, catalog_, ApiStandard::ANTHROPIC);
                            has_alt = !alt.endpoint.empty();
                        }
                    }
                }
                if (has_alt && alt.endpoint != route.endpoint) {
                    retry_after_secs_ = 0;
                    error_status      = Status::OK;
                    error_msg.clear();
                    {
                        std::lock_guard lock(data_mutex_);
                        _set_reasoning(req, ApiStandard::ANTHROPIC);
                    }
                    st = stream(
                        get_provider(alt), alt, req, cb, &retry_after_secs_);
                    const Status retried
                        = error_status != Status::OK ? error_status : st;
                    if (retried == Status::OK) {
                        std::lock_guard lock(data_mutex_);
                        if (Connection* conn
                            = _find_locked(cfg_.last_used->provider)) {
                            conn->dialects[req.model] = ApiStandard::ANTHROPIC;
                            save_config(config_path(), cfg_);
                        }
                    }
                }
            }
        }

        const Status fail = error_status != Status::OK ? error_status : st;
        if (fail == Status::RATE_LIMITED && retries < 2) {
            ++retries;
            int wait = retry_after_secs_;
            if (wait <= 0) {
                wait = retries == 1 ? 2 : 5;
            }
            wait = std::clamp(wait, 1, 30);
            _post([this, wait] {
                using namespace std::chrono_literals;
                state_.phase = UiState::Phase::CONNECTING;
                state_.retry_countdown
                    = UiState::Countdown { std::chrono::steady_clock::now()
                          + std::chrono::seconds(wait) };
            });
            using namespace std::chrono_literals;
            std::this_thread::sleep_for(std::chrono::seconds(wait));
            if (!alive_.load()) {
                return;
            }
            continue;
        }
        if (fail != Status::OK) {
            _post([this, fail, msg = error_msg] {
                state_.error.clear();
                finish(msg.empty() ? error_text(fail)
                                   : error_text(fail) + ": " + msg);
            });
            return;
        }
        retries = 0;

        if (!alive_.load()) {
            return;
        }

        std::string reply_buffer;
        const size_t history_before = history.size();
        _drain_pending_asks(history, reply_buffer, text_buffer, active_dialect);
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
    std::string& reply_buffer, const std::string& assistant_text,
    ApiStandard dialect)
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
                                "ask: expected a non-empty 'questions' "
                                "array" });
                    });
                    tool_msgs.push_back({ Message::Type::TOOL,
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
                ask.payload  = *form;
                ask.future   = std::move(future);
                ask.tool_req = ev.tool_call;
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
                            ToolCall::Result {
                                ToolCall::Result::Kind::ERROR, msg });
                    });
                    tool_msgs.push_back(
                        { Message::Type::TOOL, msg, { }, req.id });
                    continue;
                }
                const std::string text = todo_summary(*list);
                _post([this, req, todo = *list, text] {
                    state_.todo = todo;
                    _fill_tool_result(req,
                        ToolCall::Result {
                            ToolCall::Result::Kind::OUTPUT, text });
                });
                tool_msgs.push_back({ Message::Type::TOOL, text, { }, req.id });
                continue;
            }
            const Tool* tool          = tools_.find(ev.tool_call.name);
            bool needs_approval = tool != nullptr
                && tool->safety == ToolSafety::MUTATING
                && allowed_tools_.count(ev.tool_call.name) == 0;
            if (needs_approval
                && auto_approved_in_project(
                    ev.tool_call.name, ev.tool_call.args)) {
                needs_approval = false;
            }
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
                .future                   = std::move(future),
                .tool_req                 = std::nullopt });
        } else if (ev.kind == StreamEvent::Kind::QUESTION) {
            auto promise = std::make_shared<std::promise<ModalResult>>();
            auto future  = promise->get_future();
            {
                std::lock_guard lock(queue_mutex_);
                queue_.push_back(
                    PendingModal { ev.question, std::move(promise) });
            }
            asks.push_back(Ask { .payload = ev.question,
                .future                   = std::move(future),
                .tool_req                 = std::nullopt });
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
    if (dialect == ApiStandard::ANTHROPIC) {
        if (AssistantTurn* a = _last_assistant()) {
            if (!a->reasoning.empty()) {
                assistant.thinking.push_back(
                    { a->reasoning, a->reasoning_signature });
            }
        }
    }
    for (const auto& ev : stream_events_) {
        if (ev.kind == StreamEvent::Kind::TOOL_CALL) {
            assistant.tool_calls.push_back(ToolCallEntry {
                ev.tool_call.id, ev.tool_call.name, ev.tool_call.args });
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
    _post([this, copy = std::move(copy)] {
        state_.items.push_back(std::move(copy));
    });
}

void Controller::_apply_ask_result(const ToolCallRequest& req,
    const ModalResult& res, std::vector<Message>& tool_msgs)
{
    const auto* answer = std::get_if<ModalAnswer>(&res);
    if (answer == nullptr) {
        _post([this, req] {
            _fill_tool_result(
                req, ToolCall::Result { ToolCall::Result::Kind::CANCEL, "" });
        });
        tool_msgs.push_back(
            { Message::Type::TOOL, denial_text(""), { }, req.id });
        return;
    }
    ModalAnswer copy       = *answer;
    const std::string text = ask_answer_markdown(copy);
    _post([this, req, text] {
        _fill_tool_result(
            req, ToolCall::Result { ToolCall::Result::Kind::OUTPUT, text });
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
        ToolCall::Result result { kind, out.text };
        result.diff = out.diff;
        _fill_tool_result(req, std::move(result));
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
    case ToolCall::Result::Kind::CANCEL: return denial_text(call.result->text);
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
        const bool matched = !req.id.empty()
            ? tc->call_id == req.id
            : tc->name == req.name && tc->args == req.args;
        if (matched) {
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
                    if (!ev.text.empty()) {
                        _finalize_reasoning(*a);
                    }
                    a->markdown += ev.text;
                }
            }
            break;
    case StreamEvent::Kind::REASONING:
        if (!state_.items.empty()) {
            if (auto* a = std::get_if<AssistantTurn>(&state_.items.back())) {
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
        if (auto* a = std::get_if<AssistantTurn>(&state_.items.back())) {
            _finalize_reasoning(*a);
        }
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
    case StreamEvent::Kind::DONE:
        if (auto* a = _last_assistant()) {
            _finalize_reasoning(*a);
        }
        break;
    case StreamEvent::Kind::ERROR: finish(error_text(ev.error)); break;
    case StreamEvent::Kind::CONNECTED:
        if (state_.phase == UiState::Phase::CONNECTING) {
            state_.error.clear();
            state_.retry_countdown.reset();
            state_.phase = UiState::Phase::STREAMING;
        }
        break;
    case StreamEvent::Kind::USAGE: {
        std::string model;
        {
            std::lock_guard lock(data_mutex_);
            model = cfg_.last_used ? cfg_.last_used->model : "";
        }
        const ModelPricing p = get_pricing(model);
        state_.last          = ev.usage;
        state_.totals.prompt += ev.usage.prompt;
        state_.totals.completion += ev.usage.completion;
        state_.totals.total += ev.usage.total;
        state_.last_cost = compute_cost(ev.usage, p);
        state_.total_cost += state_.last_cost;
        break;
    }
    }
}

void Controller::finish(std::string error)
{
    if (state_.phase == UiState::Phase::IDLE) {
        return;
    }
    state_.retry_countdown.reset();
    if (!error.empty() && state_.error.empty()) {
        state_.error = std::move(error);
    }
    state_.phase = UiState::Phase::IDLE;
    _present_front();
    _drain_queued();
}

Config Controller::config() const
{
    std::lock_guard lock(data_mutex_);
    return cfg_;
}

std::vector<ConnectionView> Controller::connections() const
{
    std::lock_guard lock(data_mutex_);
    std::vector<ConnectionView> views;
    views.reserve(cfg_.providers.size());
    for (const Connection& conn : cfg_.providers) {
        ConnectionView view;
        view.id          = conn.id;
        view.provider_id = conn.provider_id;
        view.api_key     = conn.api_key;
        view.active = cfg_.last_used && cfg_.last_used->provider == conn.id;
        if (conn.provider_id == kLocalProviderId) {
            view.name = "Local";
        } else if (conn.provider_id == kCustomProviderId) {
            view.name = "Custom";
        } else if (const auto it = catalog_.providers.find(conn.provider_id);
            it != catalog_.providers.end()) {
            view.name = it->second.name;
        }
        if (view.name.empty()) {
            view.name = conn.provider_id;
        }
        const auto it = model_catalog_.find(conn.id);
        if (it != model_catalog_.end()) {
            if (auto* ready
                = std::get_if<CatalogEntry::Ready>(&it->second.state)) {
                view.state       = ConnectionView::State::READY;
                view.model_count = ready->models.size();
            } else if (auto* failed
                = std::get_if<CatalogEntry::Failed>(&it->second.state)) {
                view.state = ConnectionView::State::FAILED;
                view.error = failed->status;
            }
        }
        views.push_back(std::move(view));
    }
    return views;
}

ModelList Controller::models_for(const std::string& connection_id) const
{
    std::lock_guard lock(data_mutex_);
    ModelList list;
    const Connection* conn = nullptr;
    for (const Connection& conn_item : cfg_.providers) {
        if (conn_item.id == connection_id) {
            conn = &conn_item;
            break;
        }
    }
    if (conn == nullptr) {
        list.state = ModelList::State::FAILED;
        list.error = Status::CONFIG_ERROR;
        return list;
    }
    const auto it = model_catalog_.find(connection_id);
    if (it == model_catalog_.end()) {
        return list;
    }
    auto* ready = std::get_if<CatalogEntry::Ready>(&it->second.state);
    if (ready == nullptr) {
        if (auto* failed
            = std::get_if<CatalogEntry::Failed>(&it->second.state)) {
            list.state = ModelList::State::FAILED;
            list.error = failed->status;
        }
        return list;
    }
    list.state          = ModelList::State::READY;
    list.models         = ready->models;
    const auto provider = catalog_.providers.find(conn->provider_id);
    if (provider == catalog_.providers.end()) {
        return list;
    }
    for (ModelInfo& info : list.models) {
        if (info.context_length.has_value()) {
            continue;
        }
        const auto model = provider->second.models.find(info.id);
        if (model != provider->second.models.end() && model->second.context) {
            info.context_length = model->second.context;
        }
    }
    return list;
}

bool Controller::remove_connection(const std::string& connection_id)
{
    bool removed = false;
    {
        std::lock_guard lock(data_mutex_);
        if (cfg_.providers.size() <= 1) {
            return false;
        }
        auto& providers = cfg_.providers;
        providers.erase(std::remove_if(providers.begin(), providers.end(),
                            [&](const Connection& conn) {
                                return conn.id == connection_id;
                            }),
            providers.end());
        removed = true;
        ++generations_[connection_id];
        model_catalog_.erase(connection_id);
        if (cfg_.last_used && cfg_.last_used->provider == connection_id) {
            if (!cfg_.providers.empty()) {
                cfg_.last_used = LastUsed { cfg_.providers.front().id, "" };
            } else {
                cfg_.last_used.reset();
            }
        }
        save_config(config_path(), cfg_);
    }
    if (removed) {
        ++state_.modal_serial;
    }
    return removed;
}

void Controller::refetch_models(const std::string& connection_id)
{
    std::lock_guard lock(data_mutex_);
    _start_fetch_locked(connection_id);
}

std::vector<std::pair<std::string, std::string>>
Controller::provider_options() const
{
    std::lock_guard lock(data_mutex_);
    std::vector<std::pair<std::string, std::string>> options;
    options.reserve(catalog_.providers.size() + 2);
    for (const auto& [id, provider] : catalog_.providers) {
        options.emplace_back(id, provider.name.empty() ? id : provider.name);
    }
    std::sort(options.begin(), options.end());
    options.emplace_back(std::string(kLocalProviderId), "Local");
    options.emplace_back(std::string(kCustomProviderId), "Custom");
    return options;
}

Route Controller::_active_route_locked(const std::string& model) const
{
    if (!cfg_.last_used) {
        return Route { };
    }
    for (const Connection& conn : cfg_.providers) {
        if (conn.id != cfg_.last_used->provider) {
            continue;
        }
        ApiStandard dialect = ApiStandard::OPENAI;
        if (const auto it = conn.dialects.find(model);
            it != conn.dialects.end()) {
            dialect = it->second;
        }
        return resolve_route(conn, catalog_, dialect);
    }
    return Route { };
}

std::string Controller::_unique_id_locked(std::string base) const
{
    const auto taken = [&](const std::string& id) {
        for (const Connection& conn : cfg_.providers) {
            if (conn.id == id) {
                return true;
            }
        }
        return false;
    };
    if (!taken(base)) {
        return base;
    }
    for (int n = 2;; ++n) {
        std::string candidate = base + "-" + std::to_string(n);
        if (!taken(candidate)) {
            return candidate;
        }
    }
}

Connection* Controller::_find_locked(const std::string& id)
{
    for (Connection& conn : cfg_.providers) {
        if (conn.id == id) {
            return &conn;
        }
    }
    return nullptr;
}

void Controller::_start_fetch_locked(const std::string& connection_id)
{
    const Connection* conn = _find_locked(connection_id);
    if (conn == nullptr) {
        return;
    }
    const Route route = resolve_route(*conn, catalog_, ApiStandard::OPENAI);
    const int gen     = ++generations_[connection_id];
    if (route.api.empty()) {
        model_catalog_[connection_id]
            = CatalogEntry { CatalogEntry::Failed { Status::INVALID_URL } };
        return;
    }
    model_catalog_[connection_id] = CatalogEntry { CatalogEntry::Fetching { } };

    auto models = std::make_shared<std::vector<ModelInfo>>();
    ModelsFn fn = models_fn_;
    if (!fn) {
        fn = [](const Route& r, std::vector<ModelInfo>& out) {
            return fetch_models(r, out);
        };
    }
    std::shared_future<Status> future
        = std::async(std::launch::async, [fn, route, models] {
              return fn(route, *models);
          }).share();
    fetch_threads_.emplace_back([this, connection_id, gen, future, models] {
        future.wait();
        if (!alive_.load()) {
            return;
        }
        _post([this, connection_id, gen, future, models] {
            std::lock_guard lock(data_mutex_);
            if (generations_[connection_id] != gen) {
                return;
            }
            const Status st = future.get();
            if (st == Status::OK) {
                model_catalog_[connection_id]
                    = CatalogEntry { CatalogEntry::Ready { *models } };
            } else {
                model_catalog_[connection_id]
                    = CatalogEntry { CatalogEntry::Failed { st } };
            }
        });
    });
}

void Controller::_begin_connect(const ConnectResult& res)
{
    Route route;
    bool known = false;
    {
        std::lock_guard lock(data_mutex_);
        known = res.provider_id == kLocalProviderId
            || res.provider_id == kCustomProviderId
            || catalog_.providers.count(res.provider_id) > 0;
        if (known) {
            Connection probe;
            probe.provider_id = res.provider_id;
            probe.endpoint    = res.endpoint;
            probe.api_key     = res.api_key;
            route = resolve_route(probe, catalog_, ApiStandard::OPENAI);
        }
    }
    if (!known || route.endpoint.empty()) {
        state_.connect_status = "unknown provider";
        return;
    }

    auto models = std::make_shared<std::vector<ModelInfo>>();
    ModelsFn fn = models_fn_;
    if (!fn) {
        fn = [](const Route& r, std::vector<ModelInfo>& out) {
            return fetch_models(r, out);
        };
    }
    std::shared_future<Status> future
        = std::async(std::launch::async, [fn, route, models] {
              return fn(route, *models);
          }).share();
    fetch_threads_.emplace_back([this, res, future, models] {
        future.wait();
        if (!alive_.load()) {
            return;
        }
        _post([this, res, future, models] {
            const Status st = future.get();
            if (st != Status::OK) {
                state_.connect_status = error_text(st);
                return;
            }
            state_.connect_status
                = "✓ " + std::to_string(models->size()) + " models";
            if (res.persist) {
                const bool first = _commit_connection(res);
                if (std::holds_alternative<ConnectModal>(state_.modal)) {
                    state_.modal
                        = ConnectModal { first ? ConnectModal::Entry::PICK_MODEL
                                               : ConnectModal::Entry::MANAGE };
                    ++state_.modal_serial;
                }
            }
        });
    });
}

bool Controller::_commit_connection(const ConnectResult& res)
{
    std::lock_guard lock(data_mutex_);
    const bool first = !cfg_.last_used.has_value();

    Connection probe;
    probe.provider_id = res.provider_id;
    probe.endpoint    = res.endpoint;
    probe.api_key     = res.api_key;
    const Route route = resolve_route(probe, catalog_, ApiStandard::OPENAI);

    Connection stored;
    stored.provider_id = res.provider_id;
    stored.api_key     = res.api_key;
    if (res.provider_id == kLocalProviderId
        || res.provider_id == kCustomProviderId) {
        stored.endpoint = res.endpoint;
    }

    Connection* existing = nullptr;
    if (!route.endpoint.empty()) {
        for (Connection& conn : cfg_.providers) {
            const Route other
                = resolve_route(conn, catalog_, ApiStandard::OPENAI);
            if (other.endpoint == route.endpoint) {
                existing = &conn;
                break;
            }
        }
    }

    std::string id;
    if (existing != nullptr) {
        existing->provider_id = stored.provider_id;
        existing->api_key     = stored.api_key;
        existing->endpoint    = stored.endpoint;
        existing->dialects.clear();
        id = existing->id;
    } else {
        stored.id = _unique_id_locked(res.provider_id);
        id        = stored.id;
        cfg_.providers.push_back(std::move(stored));
    }
    save_config(config_path(), cfg_);
    _start_fetch_locked(id);
    return first;
}

void Controller::_apply_pick(const ModelChoice& choice)
{
    std::lock_guard lock(data_mutex_);
    if (_find_locked(choice.connection_id) == nullptr) {
        return;
    }
    cfg_.last_used = LastUsed { choice.connection_id, choice.model_id };
    save_config(config_path(), cfg_);
}

std::vector<SlashCommand> slash_commands()
{
    return {
        { "/help", "show available commands", SlashCommand::Action::HELP },
        { "/exit", "quit ursa", SlashCommand::Action::EXIT },
        { "/connect", "manage provider connections",
            SlashCommand::Action::CONNECT },
        { "/model", "pick the active model", SlashCommand::Action::MODEL },
        { "/variant", "pick reasoning effort", SlashCommand::Action::VARIANT },
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
