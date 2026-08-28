#include "controller.h"

#include "commands.h"
#include "environment.h"
#include "prompt.h"
#include "util.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace ursa {

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

Controller::Controller(std::shared_ptr<Session> session, const Config& cfg,
    PostFn post, std::function<void()> on_exit, StreamFn stream_fn,
    ToolRegistry tools, ModelsFn models_fn)
    : Controller(std::move(session),
          std::make_shared<ProviderStore>(cfg, std::move(models_fn)),
          std::move(post), std::move(on_exit), std::move(stream_fn),
          std::move(tools))
{
}

Controller::Controller(std::shared_ptr<Session> session,
    std::shared_ptr<ProviderStore> providers, PostFn post,
    std::function<void()> on_exit, StreamFn stream_fn, ToolRegistry tools)
    : session_(std::move(session))
    , post_(std::move(post))
    , on_exit_(std::move(on_exit))
    , commands_(slash_commands())
    , stream_fn_(std::move(stream_fn))
    , has_stream_override_(static_cast<bool>(stream_fn_))
    , tools_(std::move(tools))
    , providers_(std::move(providers))
{
    specs_plan_ = tools_.plan_specs();
    specs_all_  = tools_.specs();

    if (!stream_fn_) {
        stream_fn_ = [this](const ChatRequest& req, const StreamCallback& cb) {
            const auto selection = providers_->active_selection();
            const Route route
                = selection.has_value() ? selection->route : Route { };
            return stream(
                get_provider(route), route, req, cb, &retry_after_secs_);
        };
    }
    env_sub_ = get_environment()->subscribe_to_workspace_change(
        [this] {
            _post([this] {
                _present_front();
                if (session_->phase() == Session::Phase::IDLE
                    && !session_->queued().empty()) {
                    _drain_queued();
                }
            });
        });
    provider_sub_ = providers_->subscribe(
        [this] { _post([this] { session_->bump_modal_serial(); }); });
    _post([this] { providers_->start_model_fetches(); });
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
    provider_sub_();
    env_sub_();
    worker_.reset();
}

void Controller::toggle_mode() { session_->toggle_mode(); }

void Controller::set_error(std::string msg)
{
    session_->set_error(std::move(msg));
}

void Controller::clear_error() { session_->clear_error(); }

void Controller::close_modal() { resolve_modal(std::monostate { }); }

void Controller::enqueue_user_modal(ModalPayload payload)
{
    {
        std::lock_guard lock(queue_mutex_);
        queue_.push_back(PendingModal { std::move(payload),
            std::shared_ptr<std::promise<ModalResult>> { } });
    }
    if (session_->modal().index() == 0
        && (session_->phase() == Session::Phase::IDLE
            || session_->phase() == Session::Phase::CONNECTING
            || session_->phase() == Session::Phase::STREAMING)) {
        _present_front();
    }
}

void Controller::cancel_queued(std::size_t id) { session_->cancel_queued(id); }

void Controller::interrupt()
{
    if (session_->phase() != Session::Phase::IDLE) {
        session_->request_interrupt();
    }
}

void Controller::_drain_queued()
{
    std::optional<QueuedMessage> next = session_->pop_queued();
    if (!next.has_value()) {
        return;
    }
    submit(std::move(next->text), std::move(next->attachments));
}

size_t Controller::queue_size() const
{
    std::lock_guard lock(queue_mutex_);
    return queue_.size();
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
        providers_->set_reasoning_effort(variant->effort);
        session_->bump_modal_serial();
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
    session_->clear_modal();
    if (session_->phase() == Session::Phase::AWAITING) {
        session_->set_phase(Session::Phase::CONNECTING);
    }
    _present_front();
}

void Controller::_present_front()
{
    std::lock_guard lock(queue_mutex_);
    if (session_->modal().index() != 0 || queue_.empty()) {
        return;
    }
    session_->present_modal(queue_.front().payload);
}

void Controller::submit(
    std::string text, std::vector<FileAttachment> attachments)
{
    const std::string_view t = trim(text);
    if (t.empty()) {
        return;
    }
    if (t[0] == '/') {
        run_slash(t);
        return;
    }
    if (session_->phase() == Session::Phase::IDLE) {
        submit_message(std::string(t), std::move(attachments));
    } else {
        session_->enqueue_message(std::string(t), std::move(attachments));
    }
}

void Controller::submit_message(
    std::string text, std::vector<FileAttachment> attachments)
{
    const std::optional<ProviderSelection> selection
        = providers_->active_selection();
    if (!selection.has_value()) {
        session_->set_error("no model selected — run /model");
        return;
    }
    TurnSettings settings;
    settings.model            = selection->model;
    settings.connection_id    = selection->connection_id;
    settings.reasoning_effort = selection->reasoning_effort;
    if (settings.reasoning_effort == "medium") {
        settings.reasoning_effort = "default";
    }
    settings.route   = selection->route;
    settings.dialect = settings.route.dialect;
    settings.mode    = session_->mode();
    if (!get_environment()->ready()) {
        session_->enqueue_message(std::move(text), std::move(attachments));
        return;
    }
    const bool generate_title = session_->claim_title_generation();
    const std::string title_input = text;
    session_->clear_interrupt();
    session_->begin_send(std::move(text), std::move(attachments));
    _spawn(session_->build_history(_system_prompt(), settings.dialect),
        has_stream_override_ ? stream_fn_ : StreamFn { }, std::move(settings));
    if (generate_title && !has_stream_override_) {
        TurnSettings title_settings;
        title_settings.model         = selection->model;
        title_settings.connection_id = selection->connection_id;
        title_settings.route         = selection->route;
        title_settings.dialect       = selection->route.dialect;
        _spawn_title(title_input, std::move(title_settings));
    }
}

std::string Controller::_system_prompt() const
{
    const std::shared_ptr<Environment> env = get_environment();
    return build_system_prompt(env->system().get(), env->workspace().get());
}

bool Controller::_model_reasons(const std::string& model) const
{
    if (model.empty()) {
        return false;
    }
    return providers_->model_reasons(model);
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

void Controller::_set_reasoning(
    ChatRequest& req, ApiStandard dialect, std::string_view configured_effort)
{
    req.reasoning_effort.reset();
    req.thinking_budget.reset();
    std::string effort(configured_effort);
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

void Controller::_post(std::function<void()> f)
{
    if (alive_.load()) {
        post_(std::move(f));
    }
}

void Controller::_spawn(
    std::vector<Message> history, StreamFn override, TurnSettings settings)
{
    session_->append_assistant(settings.model, settings.reasoning_effort);
    worker_.emplace([this, history = std::move(history),
                        override = std::move(override),
                        settings = std::move(settings)]() mutable {
        _drive(std::move(history), std::move(override), std::move(settings));
    });
}

void Controller::_spawn_title(std::string input, TurnSettings settings)
{
    title_worker_.emplace([this, input = std::move(input),
                              settings = std::move(settings)] {
        ChatRequest req;
        req.model       = settings.model;
        req.temperature = 0.2;
        req.interrupted = [] { return false; };
        req.messages = {
            { Message::Type::SYSTEM,
                "Create a concise 3-7 word title for the user's request. "
                "Return only the title, without quotes or punctuation." },
            { Message::Type::USER, input },
        };
        std::string title;
        const StreamCallback cb = [&](const StreamEvent& event) {
            if (event.kind == StreamEvent::Kind::CONTENT_DELTA
                && title.size() < 200) {
                title += event.text;
            }
        };
        Route route = settings.route;
        const Status status
            = stream(get_provider(route), route, req, cb, nullptr);
        if (status != Status::OK) {
            return;
        }
        title = std::string(trim(title));
        const std::size_t newline = title.find_first_of("\r\n");
        if (newline != std::string::npos) {
            title.erase(newline);
        }
        if (title.size() >= 2
            && ((title.front() == '"' && title.back() == '"')
                || (title.front() == '\'' && title.back() == '\''))) {
            title = title.substr(1, title.size() - 2);
        }
        if (title.empty()) {
            return;
        }
        _post([this, title = std::move(title)]() mutable {
            session_->set_title(std::move(title));
        });
    });
}

void Controller::finish(std::string error)
{
    const bool ended = session_->finish_session(std::move(error));
    if (!ended) {
        return;
    }
    _present_front();
    _drain_queued();
}

void Controller::_begin_connect(const ConnectResult& result)
{
    providers_->connect(result, [this](ConnectOutcome outcome) {
        _post([this, outcome] {
            if (outcome.status != Status::OK) {
                session_->set_connect_status(error_text(outcome.status));
                return;
            }
            session_->set_connect_status(
                "✓ " + std::to_string(outcome.model_count) + " models");
            if (outcome.persisted
                && std::holds_alternative<ConnectModal>(session_->modal())) {
                session_->set_modal(ConnectModal { outcome.first_connection
                        ? ConnectModal::Entry::PICK_MODEL
                        : ConnectModal::Entry::MANAGE });
                session_->bump_modal_serial();
            }
        });
    });
}

void Controller::_apply_pick(const ModelChoice& choice)
{
    providers_->select_model(choice);
}

} // namespace ursa
