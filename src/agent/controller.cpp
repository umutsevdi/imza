#include "controller.h"

#include "commands.h"
#include "environment.h"
#include "pricing.h"
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
    : session_(std::move(session))
    , cfg_(cfg)
    , post_(std::move(post))
    , on_exit_(std::move(on_exit))
    , commands_(slash_commands())
    , stream_fn_(std::move(stream_fn))
    , tools_(std::move(tools))
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
    env_sub_ = get_environment()->subscribe_to_workspace_change(
        [this](std::shared_ptr<const WorkspaceEnvironment>) {
            _post([this] {
                _present_front();
                if (session_->phase() == Session::Phase::IDLE
                    && !session_->queued().empty()) {
                    _drain_queued();
                }
            });
        });
    _post([this] {
        std::lock_guard lock(data_mutex_);
        for (const Connection& conn : cfg_.providers) {
            _start_fetch_locked(conn.id);
        }
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
    catalog_waiter_.reset();
    fetch_threads_.clear();
    env_sub_();
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

void Controller::cancel_queued(std::size_t id)
{
    session_->cancel_queued(id);
}

void Controller::_drain_queued()
{
    std::optional<QueuedMessage> next = session_->pop_queued();
    if (!next.has_value()) {
        return;
    }
    submit(std::move(next->text));
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
        {
            std::lock_guard lock(data_mutex_);
            cfg_.reasoning_effort = variant->effort;
            save_config(config_path(), cfg_);
        }
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
    if (session_->phase() == Session::Phase::IDLE) {
        submit_message(std::string(t));
    } else {
        session_->enqueue_message(std::string(t));
    }
}

void Controller::submit_message(std::string text)
{
    {
        std::lock_guard lock(data_mutex_);
        if (!cfg_.last_used || cfg_.last_used->model.empty()) {
            session_->set_error("no model selected — run /model");
            return;
        }
    }
    if (!get_environment()->ready()) {
        session_->enqueue_message(std::move(text));
        return;
    }
    session_->begin_send(std::move(text));
    _spawn(session_->build_history(_system_prompt()), StreamFn { });
}

std::string Controller::_system_prompt() const
{
    const std::shared_ptr<Environment> env = get_environment();
    return build_system_prompt(
        env->system().get(), env->workspace().get());
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

void Controller::_post(std::function<void()> f)
{
    if (alive_.load()) {
        post_(std::move(f));
    }
}

void Controller::_spawn(std::vector<Message> history, StreamFn override)
{
    session_->append_assistant();
    worker_.emplace([this, history = std::move(history),
                        override = std::move(override)]() mutable {
        _drive(std::move(history), std::move(override));
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

} // namespace ursa