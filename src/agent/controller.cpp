#include "controller.h"

#include "environment.h"
#include "prompt.h"
#include "session_store.h"
#include "util.h"

#include <functional>
#include <cctype>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <fstream>
#include <sstream>

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
    std::vector<Tool> tools, ModelsFn models_fn)
    : Controller(std::make_shared<ApplicationState>(ApplicationState {
          std::move(session),
          std::make_shared<ProviderStore>(cfg, std::move(models_fn)),
          std::make_shared<SubagentManager>(), get_environment() }),
          std::move(post), std::move(on_exit), std::move(stream_fn),
          std::move(tools))
{
}

Controller::Controller(std::shared_ptr<Session> session,
    std::shared_ptr<ProviderStore> providers, PostFn post,
    std::function<void()> on_exit, StreamFn stream_fn,
    std::vector<Tool> tools)
    : Controller(std::make_shared<ApplicationState>(ApplicationState {
          std::move(session), std::move(providers),
          std::make_shared<SubagentManager>(), get_environment() }),
          std::move(post), std::move(on_exit), std::move(stream_fn),
          std::move(tools))
{
}

Controller::Controller(std::shared_ptr<ApplicationState> state, PostFn post,
    std::function<void()> on_exit, StreamFn stream_fn,
    std::vector<Tool> tools, ModalRequestFn modal_request,
    std::string agent_label)
    : state_(std::move(state))
    , post_(std::move(post))
    , on_exit_(std::move(on_exit))
    , modal_request_(std::move(modal_request))
    , agent_label_(std::move(agent_label))
    , stream_fn_(std::move(stream_fn))
    , has_stream_override_(static_cast<bool>(stream_fn_))
    , tools_(std::move(tools))
{
    for (Tool& tool : tools_) {
        if (tool.spec.name != "skill") continue;
        tool.run = [this](const Json::Value& args) {
            const auto skill = _resolve_skill(args);
            if (!skill) return ToolOutput { ToolOutput::Kind::ERROR, "skill: unknown or unavailable skill" };
            if (_skill_policy(*skill) == SkillPolicy::DENY)
                return ToolOutput { ToolOutput::Kind::ERROR, "skill: access denied by configuration" };
            std::ifstream file(skill->path, std::ios::binary);
            if (!file) return ToolOutput { ToolOutput::Kind::ERROR, "skill: cannot read instructions" };
            std::ostringstream buffer;
            buffer << file.rdbuf();
            std::string content = buffer.str();
            constexpr std::size_t max_skill_bytes = 128 * 1024;
            if (content.size() > max_skill_bytes)
                return ToolOutput { ToolOutput::Kind::ERROR, "skill: instructions exceed 128 KiB" };
            return ToolOutput { ToolOutput::Kind::OUTPUT,
                "<skill name=\"" + skill->name + "\" directory=\"" + skill->path.parent_path().string() + "\">\n" + content + "\n</skill>" };
        };
        break;
    }
    specs_plan_ = plan_tool_specs(tools_);
    specs_all_  = tool_specs(tools_);

    if (!stream_fn_) {
        stream_fn_ = [this](const ChatRequest& req, const StreamCallback& cb) {
            const auto selection = state_->providers->active_selection();
            const Route route
                = selection.has_value() ? selection->route : Route { };
            return stream(
                get_provider(route), route, req, cb, &retry_after_secs_);
        };
    }
    env_sub_ = state_->environment->subscribe_to_workspace_change(
        [this] {
            _post([this] {
                _present_front();
                if (state_->session->phase() == Session::Phase::IDLE
                    && !state_->session->queued().empty()) {
                    _drain_queued();
                }
            });
        });
    provider_sub_ = state_->providers->subscribe(
        [this] { _post([this] { state_->session->bump_modal_serial(); }); });
    _post([this] { state_->providers->start_model_fetches(); });
}

std::optional<Skill> Controller::_resolve_skill(const Json::Value& args) const
{
    if (!args.isObject() || !args["name"].isString()) return std::nullopt;
    const std::string name = args["name"].asString();
    const std::string scope = args["scope"].isString() ? args["scope"].asString() : "";
    std::optional<Skill> global;
    for (const Skill& skill : state_->environment->skills()) {
        if (skill.name != name) continue;
        if (scope == "project" && skill.scope != Skill::Scope::PROJECT) continue;
        if (scope == "global" && skill.scope != Skill::Scope::GLOBAL) continue;
        if (skill.scope == Skill::Scope::PROJECT) return skill;
        global = skill;
    }
    return global;
}

SkillPolicy Controller::_skill_policy(const Skill& skill) const
{
    const Config config = state_->providers->config();
    if (skill.scope == Skill::Scope::GLOBAL) {
        if (auto it = config.global_skills.find(skill.name); it != config.global_skills.end()) return it->second;
    } else if (skill.project_root) {
        if (auto project = config.project_skills.find(skill.project_root->string()); project != config.project_skills.end())
            if (auto it = project->second.find(skill.name); it != project->second.end()) return it->second;
    }
    return SkillPolicy::ASK;
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
    provider_sub_.disconnect();
    env_sub_.disconnect();
    state_->subagents->stop();
    worker_.reset();
}

void Controller::set_mode(Session::Mode next_mode)
{
    state_->session->set_mode(next_mode);
}

void Controller::set_error(std::string msg)
{
    state_->session->set_error(std::move(msg));
}

void Controller::clear_error() { state_->session->clear_error(); }

void Controller::close_modal() { resolve_modal(std::monostate { }); }

void Controller::enqueue_user_modal(ModalPayload payload)
{
    {
        std::lock_guard lock(queue_mutex_);
        queue_.push_back(PendingModal { std::move(payload),
            std::shared_ptr<std::promise<ModalResult>> { } });
    }
    if (state_->session->modal().index() == 0
        && (state_->session->phase() == Session::Phase::IDLE
            || state_->session->phase() == Session::Phase::CONNECTING
            || state_->session->phase() == Session::Phase::STREAMING)) {
        _present_front();
    }
}

std::future<ModalResult> Controller::_request_modal(ModalPayload payload)
{
    if (!agent_label_.empty()) {
        if (auto* request = std::get_if<ToolCallRequest>(&payload)) {
            request->description = agent_label_ + " · "
                + (request->description.empty() ? request->name
                                                : request->description);
        } else if (auto* form = std::get_if<QuestionForm>(&payload)) {
            if (!form->empty()) {
                form->front().prompt
                    = agent_label_ + " · " + form->front().prompt;
            }
        }
    }
    if (modal_request_) return modal_request_(std::move(payload));

    auto promise = std::make_shared<std::promise<ModalResult>>();
    auto future  = promise->get_future();
    {
        std::lock_guard lock(queue_mutex_);
        queue_.push_back(PendingModal { std::move(payload), promise });
    }
    _post([this] { _present_front(); });
    return future;
}

void Controller::cancel_queued(std::size_t id) { state_->session->cancel_queued(id); }

SessionsModal Controller::_sessions_modal() const
{
    SessionsModal modal;
    for (const auto& saved : saved_sessions()) {
        modal.titles.push_back(saved.title);
        modal.saved_at.push_back(saved.saved_at);
        modal.paths.push_back(saved.path.string());
    }
    return modal;
}

SkillsModal Controller::_skills_modal() const
{
    SkillsModal modal;
    const Config config = state_->providers->config();
    for (const Skill& skill : state_->environment->skills()) {
        SkillPolicy policy = SkillPolicy::ASK;
        std::string root;
        if (skill.scope == Skill::Scope::GLOBAL) {
            if (auto it = config.global_skills.find(skill.name); it != config.global_skills.end()) policy = it->second;
        } else if (skill.project_root) {
            root = skill.project_root->string();
            if (auto p = config.project_skills.find(root); p != config.project_skills.end())
                if (auto it = p->second.find(skill.name); it != p->second.end()) policy = it->second;
        }
        modal.entries.push_back({ skill.name, skill.description, root, policy });
    }
    return modal;
}

void Controller::delete_saved_session(const std::filesystem::path& path)
{
    std::error_code ec;
    const std::filesystem::path root
        = std::filesystem::weakly_canonical(sessions_dir(), ec);
    const std::filesystem::path target
        = std::filesystem::weakly_canonical(path, ec);
    if (ec || target.parent_path() != root || target.extension() != ".json") {
        state_->session->set_error("invalid session path");
        return;
    }
    if (!std::filesystem::remove(target, ec) || ec) {
        state_->session->set_error("failed to delete session");
        return;
    }
    state_->session->set_modal(_sessions_modal());
    state_->session->bump_modal_serial();
}

void Controller::interrupt()
{
    if (state_->session->phase() != Session::Phase::IDLE) {
        state_->session->request_interrupt();
    }
}

void Controller::_drain_queued()
{
    std::optional<QueuedMessage> next = state_->session->pop_queued();
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

std::pair<SkillCounts, SkillCounts> Controller::skill_counts() const
{
    SkillCounts project;
    SkillCounts global;
    std::lock_guard lock(loaded_skills_mutex_);
    for (const Skill& skill : state_->environment->skills()) {
        SkillCounts& counts = skill.scope == Skill::Scope::PROJECT
            ? project
            : global;
        ++counts.total;
        if (loaded_skills_.contains(skill.path.string())) {
            ++counts.active;
        }
    }
    return { project, global };
}

std::vector<Skill> Controller::available_skills() const
{
    std::vector<Skill> out;
    for (const Skill& skill : state_->environment->skills()) {
        if (_skill_policy(skill) != SkillPolicy::DENY) out.push_back(skill);
    }
    return out;
}

bool Controller::_validate_skill_mentions(std::string_view text)
{
    for (std::size_t pos = 0; pos < text.size();) {
        pos = text.find('$', pos);
        if (pos == std::string_view::npos) break;
        if (pos > 0 && !std::isspace(static_cast<unsigned char>(text[pos - 1]))) {
            ++pos;
            continue;
        }
        std::size_t end = pos + 1;
        while (end < text.size()) {
            const unsigned char c = static_cast<unsigned char>(text[end]);
            if (!std::isalnum(c) && c != '-' && c != '_') break;
            ++end;
        }
        if (end == pos + 1) {
            ++pos;
            continue;
        }
        Json::Value args(Json::objectValue);
        args["name"] = std::string(text.substr(pos + 1, end - pos - 1));
        const auto skill = _resolve_skill(args);
        if (!skill) {
            state_->session->set_error("unknown skill: " + args["name"].asString());
            return false;
        }
        if (_skill_policy(*skill) == SkillPolicy::DENY) {
            state_->session->set_error("skill is denied: " + skill->name);
            return false;
        }
        pos = end;
    }
    return true;
}

std::vector<Skill> Controller::_mentioned_skills(std::string_view text) const
{
    std::vector<Skill> out;
    std::set<std::string> paths;
    for (std::size_t pos = 0; pos < text.size();) {
        pos = text.find('$', pos);
        if (pos == std::string_view::npos) break;
        if (pos > 0 && !std::isspace(static_cast<unsigned char>(text[pos - 1]))) {
            ++pos;
            continue;
        }
        std::size_t end = pos + 1;
        while (end < text.size()) {
            const unsigned char c = static_cast<unsigned char>(text[end]);
            if (!std::isalnum(c) && c != '-' && c != '_') break;
            ++end;
        }
        if (end > pos + 1) {
            Json::Value args(Json::objectValue);
            args["name"] = std::string(text.substr(pos + 1, end - pos - 1));
            if (const auto skill = _resolve_skill(args);
                skill && paths.insert(skill->path.string()).second) {
                out.push_back(*skill);
            }
        }
        pos = end;
    }
    return out;
}

bool Controller::_load_skill(const Skill& skill)
{
    {
        std::lock_guard lock(loaded_skills_mutex_);
        if (loaded_skills_.contains(skill.path.string())) return true;
    }
    std::ifstream file(skill.path, std::ios::binary);
    if (!file) {
        state_->session->set_error("failed to read skill: " + skill.name);
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    constexpr std::size_t max_skill_bytes = 128 * 1024;
    if (content.size() > max_skill_bytes) {
        state_->session->set_error("skill instructions exceed 128 KiB: " + skill.name);
        return false;
    }
    std::lock_guard lock(loaded_skills_mutex_);
    loaded_skills_.insert(skill.path.string());
    loaded_skill_contents_[skill.path.string()] = "<skill name=\""
        + skill.name + "\" directory=\"" + skill.path.parent_path().string()
        + "\">\n" + content + "\n</skill>";
    return true;
}

void Controller::resolve_modal(ModalResult result)
{
    bool manual_skill = false;
    bool manual_accepted = false;
    const ModalPayload current_modal = state_->session->modal();
    if (const auto* request = std::get_if<ToolCallRequest>(&current_modal);
        request != nullptr && request->id == "manual-skill") {
        manual_skill = true;
        const auto* verdict = std::get_if<ToolVerdict>(&result);
        manual_accepted = verdict != nullptr
            && verdict->decision != ToolDecision::REJECT;
        if (manual_accepted && pending_skill_turn_) {
            manual_accepted = _load_skill(
                pending_skill_turn_->awaiting[pending_skill_turn_->next]);
        }
    }
    if (auto* path = std::get_if<std::filesystem::path>(&result)) {
        if (state_->session->has_pending_work()) {
            state_->session->set_error(
                "finish or interrupt pending work before loading a session");
            return;
        }
        if (save_session(*state_->session) != Status::OK) {
            state_->session->set_error("failed to save current session");
            return;
        }
        if (load_session(*path, *state_->session) != Status::OK) {
            state_->session->set_error("failed to load session");
        }
    }
    if (auto* connect = std::get_if<ConnectResult>(&result)) {
        _begin_connect(*connect);
        return;
    }
    if (auto* choice = std::get_if<ModelChoice>(&result)) {
        _apply_pick(*choice);
    }
    if (auto* variant = std::get_if<VariantChoice>(&result)) {
        state_->providers->set_reasoning_effort(variant->effort);
        state_->session->bump_modal_serial();
    }
    if (auto* skills = std::get_if<SkillPolicyChanges>(&result)) {
        if (!state_->providers->set_skill_policies(*skills)) {
            state_->session->set_error("failed to save skill policies");
            return;
        }
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
    state_->session->clear_modal();
    if (state_->session->phase() == Session::Phase::AWAITING) {
        state_->session->set_phase(Session::Phase::CONNECTING);
    }
    _present_front();
    if (!manual_skill) return;
    if (!manual_accepted || !pending_skill_turn_) {
        pending_skill_turn_.reset();
        state_->session->set_error("skill activation cancelled");
        return;
    }
    ++pending_skill_turn_->next;
    if (pending_skill_turn_->next < pending_skill_turn_->awaiting.size()) {
        const Skill& skill
            = pending_skill_turn_->awaiting[pending_skill_turn_->next];
        Json::Value args(Json::objectValue);
        args["name"] = skill.name;
        args["scope"] = skill.scope == Skill::Scope::PROJECT
            ? "project"
            : "global";
        enqueue_user_modal(ToolCallRequest { "skill", write_json(args),
            "Load skill " + skill.name, "manual-skill",
            ToolCallRequest::ApprovalReason::TOOL_PERMISSION });
        return;
    }
    PendingSkillTurn turn = std::move(*pending_skill_turn_);
    pending_skill_turn_.reset();
    submit_message(std::move(turn.text), std::move(turn.attachments));
}

void Controller::_present_front()
{
    std::lock_guard lock(queue_mutex_);
    if (state_->session->modal().index() != 0 || queue_.empty()) {
        return;
    }
    state_->session->present_modal(queue_.front().payload);
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
    if (state_->session->phase() == Session::Phase::IDLE) {
        _submit_with_skills(std::string(t), std::move(attachments));
    } else {
        state_->session->enqueue_message(std::string(t), std::move(attachments));
    }
}

void Controller::_submit_with_skills(
    std::string text, std::vector<FileAttachment> attachments)
{
    if (!_validate_skill_mentions(text)) return;
    std::vector<Skill> awaiting;
    for (const Skill& skill : _mentioned_skills(text)) {
        {
            std::lock_guard lock(loaded_skills_mutex_);
            if (loaded_skills_.contains(skill.path.string())) continue;
        }
        if (_skill_policy(skill) == SkillPolicy::ALLOW) {
            if (!_load_skill(skill)) return;
        } else {
            awaiting.push_back(skill);
        }
    }
    if (awaiting.empty()) {
        submit_message(std::move(text), std::move(attachments));
        return;
    }
    pending_skill_turn_ = PendingSkillTurn { std::move(text),
        std::move(attachments), std::move(awaiting), 0 };
    const Skill& skill = pending_skill_turn_->awaiting.front();
    Json::Value args(Json::objectValue);
    args["name"] = skill.name;
    args["scope"] = skill.scope == Skill::Scope::PROJECT ? "project" : "global";
    enqueue_user_modal(ToolCallRequest { "skill", write_json(args),
        "Load skill " + skill.name, "manual-skill",
        ToolCallRequest::ApprovalReason::TOOL_PERMISSION });
}

void Controller::submit_message(
    std::string text, std::vector<FileAttachment> attachments)
{
    const std::optional<ProviderSelection> selection
        = state_->providers->active_selection();
    if (!selection.has_value()) {
        state_->session->set_error("no model selected — run /model");
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
    settings.mode    = state_->session->mode();
    if (!state_->environment->ready()) {
        state_->session->enqueue_message(std::move(text), std::move(attachments));
        return;
    }
    const bool generate_title = state_->session->claim_title_generation();
    const std::string title_input = text;
    state_->session->clear_interrupt();
    state_->session->begin_send(std::move(text), std::move(attachments));
    _spawn(state_->session->build_history(_system_prompt(), settings.dialect),
        has_stream_override_ ? stream_fn_ : StreamFn { }, std::move(settings));
    if (generate_title && !has_stream_override_) {
        const auto title_selection
            = state_->providers->subagent_selection(SubagentRole::BASIC);
        const ProviderSelection& selected
            = title_selection ? *title_selection : *selection;
        TurnSettings title_settings;
        title_settings.model         = selected.model;
        title_settings.connection_id = selected.connection_id;
        title_settings.reasoning_effort = selected.reasoning_effort;
        title_settings.route         = selected.route;
        title_settings.dialect       = selected.route.dialect;
        _spawn_title(title_input, std::move(title_settings));
    }
}

std::string Controller::_system_prompt() const
{
    const std::shared_ptr<Environment> env = state_->environment;
    const Config config = state_->providers->config();
    std::string prompt = build_system_prompt(
        env->system().get(), env->workspace().get(), &config);
    std::lock_guard lock(loaded_skills_mutex_);
    for (const auto& [path, content] : loaded_skill_contents_) {
        prompt += "\n\n" + content;
    }
    return prompt;
}

bool Controller::_model_reasons(const std::string& model) const
{
    if (model.empty()) {
        return false;
    }
    return state_->providers->model_reasons(model);
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
    state_->session->append_assistant(settings.model, settings.reasoning_effort);
    worker_.emplace([this, history = std::move(history),
                        override = std::move(override),
                        settings = std::move(settings)]() mutable {
        _drive(std::move(history), std::move(override), std::move(settings));
    });
}

void Controller::_spawn_title(std::string input, TurnSettings settings)
{
    const std::string prompt
        = "Create a concise 3-7 word title for the user's request. Return only "
          "the title, without quotes or punctuation.\n\nUser request:\n"
        + input;
    state_->subagents->start(prompt, settings.model, settings.reasoning_effort, false,
        [this, prompt, settings = std::move(settings)](std::stop_token stop) {
        ChatRequest req;
        req.model       = settings.model;
        req.temperature = 0.2;
        req.interrupted = [stop] { return stop.stop_requested(); };
        req.messages    = { { Message::Type::USER, prompt } };
        _set_reasoning(req, settings.dialect, settings.reasoning_effort);
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
        if (status != Status::OK) return SubagentResult { status, { } };
        return SubagentResult { Status::OK, std::move(title) };
    }, [this](const SubagentResult& result) {
        if (result.status != Status::OK) return;
        std::string title = std::string(trim(result.output));
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
            state_->session->set_title(std::move(title));
        });
    });
}

SubagentHandle Controller::run_subagent(std::string prompt, std::string model,
    std::string variant, bool visible)
{
    const auto selection = state_->providers->active_selection();
    Route route = selection ? selection->route : Route { };
    if (model.empty() && selection) model = selection->model;
    if (variant.empty() && selection) variant = selection->reasoning_effort;
    const std::string task_prompt = prompt;
    return state_->subagents->start(std::move(prompt), model, variant, visible,
        [this, task_prompt, model = std::move(model),
            variant = std::move(variant), route = std::move(route)](
            std::stop_token stop) mutable {
            if (model.empty() || route.api.empty()) {
                return SubagentResult { Status::CONFIG_ERROR, { } };
            }
            ChatRequest req;
            req.model       = model;
            req.interrupted = [stop] { return stop.stop_requested(); };
            req.messages = { { Message::Type::SYSTEM, _system_prompt() },
                { Message::Type::USER, task_prompt } };
            _set_reasoning(req, route.dialect, variant);
            std::string output;
            const StreamCallback callback = [&](const StreamEvent& event) {
                if (event.kind == StreamEvent::Kind::CONTENT_DELTA) {
                    output += event.text;
                }
            };
            const Status status = has_stream_override_
                ? stream_fn_(req, callback)
                : stream(get_provider(route), route, req, callback, nullptr);
            return SubagentResult { status, std::move(output) };
        });
}

void Controller::finish(std::string error)
{
    const bool ended = state_->session->finish_session(std::move(error));
    if (!ended) {
        return;
    }
    _present_front();
    _drain_queued();
}

void Controller::_begin_connect(const ConnectResult& result)
{
    state_->providers->connect(result, [this](ConnectOutcome outcome) {
        _post([this, outcome] {
            if (outcome.status != Status::OK) {
                state_->session->set_connect_status(error_text(outcome.status));
                return;
            }
            state_->session->set_connect_status(
                "✓ " + std::to_string(outcome.model_count) + " models");
            if (outcome.persisted
                && std::holds_alternative<ConnectModal>(state_->session->modal())) {
                state_->session->set_modal(ConnectModal { outcome.first_connection
                        ? ConnectModal::Entry::PICK_MODEL
                        : ConnectModal::Entry::MANAGE });
                state_->session->bump_modal_serial();
            }
        });
    });
}

void Controller::_apply_pick(const ModelChoice& choice)
{
    state_->providers->select_model(choice);
}

void Controller::run_slash(std::string_view command)
{
    run_slash_command(SlashCommandContext { *state_, on_exit_,
                          [this] { _new_session(); },
                          [this](ModalPayload payload) {
                              enqueue_user_modal(std::move(payload));
                          },
                          [this] { return _sessions_modal(); },
                          [this] { return _skills_modal(); },
                          [this] { return _system_prompt(); },
                          [this](std::string error) {
                              set_error(std::move(error));
                          } },
        command);
}

void Controller::_new_session()
{
    if (state_->session->has_pending_work()) {
        state_->session->set_error(
            "finish or interrupt pending work before starting a new session");
        return;
    }
    if (save_session(*state_->session) != Status::OK) {
        state_->session->set_error("failed to save current session");
        return;
    }
    {
        std::lock_guard lock(queue_mutex_);
        queue_.clear();
    }
    {
        std::lock_guard lock(loaded_skills_mutex_);
        loaded_skills_.clear();
        loaded_skill_contents_.clear();
    }
    allowed_tools_.clear();
    pending_skill_turn_.reset();
    stream_events_.clear();
    state_->session->restore(SessionSnapshot { });
}

} // namespace ursa
