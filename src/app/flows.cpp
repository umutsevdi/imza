#include "app/flows.h"
#include "app/slash_commands.h"
#include "common/util.h"
#include "conversation/persistence.h"
#include "network/json_io.h"
#include "permissions/evaluator.h"
#include "permissions/store.h"
#include "platform/config.h"
#include "tools/skills.h"
#include "turn/delegation.h"
#include "turn/prompt.h"
#include "turn/turn_runner.h"

#include <memory>
#include <string>
#include <utility>

namespace imza {

namespace {
    void notify_user(ApplicationState& state, AgentNotification notification)
    {
        if (state.agent_label.empty()
            && (state.runtime_flags & RuntimeFlag::ATTENDED)
                != RuntimeFlag::NONE
            && state.notify_user) {
            state.notify_user(notification);
        }
    }

    bool change_directory(
        ApplicationState& state, const std::filesystem::path& directory)
    {
        std::error_code error;
        const std::filesystem::path canonical
            = std::filesystem::weakly_canonical(directory, error);
        if (error) {
            return false;
        }
        switch (state.environment->chdir(canonical)) {
        case Environment::ChdirResult::FAILED: return false;
        case Environment::ChdirResult::UNCHANGED: return true;
        case Environment::ChdirResult::CHANGED: break;
        }
        state.permissions->clear();
        return true;
    }

    void start_turn(ApplicationState& state, std::string text,
        std::vector<FileAttachment> attachments);

    bool load_skill(ApplicationState& state, const Skill& skill,
        const ToolCallRequest& authorized)
    {
        const std::optional<std::filesystem::path> path
            = canonical_skill_path(skill);
        const Json::Value arguments = parse_json(authorized.args);
        if (!path || !arguments["path"].isString()
            || arguments["path"].asString() != path->string()) {
            state.session->set_error(
                "Skill permission target changed before activation.");
            return false;
        }
        std::string error;
        if (!state.skills->load(skill, error)) {
            state.session->set_error(std::move(error));
            return false;
        }
        return true;
    }

    ToolCallRequest skill_request(const Skill& skill)
    {
        Json::Value args(Json::objectValue);
        args["name"] = skill.name;
        args["scope"]
            = skill.scope == Skill::Scope::PROJECT ? "project" : "global";
        return { "skill", write_json(args), "Load skill " + skill.name,
            "manual-skill", "", false };
    }

    PermissionEvaluation evaluate_permission(ApplicationState& state,
        const ToolCallRequest& request, Session::Mode mode)
    {
        return evaluate_tool_request(request,
            permission_context(*state.environment, *state.permissions, mode),
            state.providers->config(), state.environment->skills(),
            *state.skills);
    }

    void submit_with_skills(ApplicationState& state, std::string text,
        std::vector<FileAttachment> attachments)
    {
        const std::vector<Skill> catalog = state.environment->skills();
        std::vector<Skill> awaiting;
        for (const Skill& skill : mentioned_skills(catalog, text)) {
            const PermissionEvaluation evaluation = evaluate_permission(
                state, skill_request(skill), state.session->mode());
            if (evaluation.decision.kind == PermissionDecision::Kind::ACCEPT
                || (state.runtime_flags & RuntimeFlag::SKIP_PERMISSIONS)
                    != RuntimeFlag::NONE) {
                if (!load_skill(state, skill, evaluation.request)) {
                    return;
                }
                continue;
            }
            if ((state.runtime_flags & RuntimeFlag::ATTENDED)
                == RuntimeFlag::NONE) {
                state.session->set_error(
                    "Skill permission is unavailable in unattended mode.");
                return;
            }
            awaiting.push_back(skill);
        }
        if (awaiting.empty()) {
            start_turn(state, std::move(text), std::move(attachments));
            return;
        }
        const Skill first = awaiting.front();
        state.skills->set_pending_turn(PendingSkillTurn {
            std::move(text), std::move(attachments), std::move(awaiting), 0 });
        enqueue_user_modal(state,
            evaluate_permission(
                state, skill_request(first), state.session->mode())
                .request);
    }

    void start_turn(ApplicationState& state, std::string text,
        std::vector<FileAttachment> attachments)
    {
        const std::optional<ProviderSelection> selection
            = state.providers->active_selection();
        if (!selection.has_value()) {
            state.session->set_error("No model selected - run /model.");
            return;
        }
        const TurnSettings settings
            = make_turn_settings(*selection, state.session->mode());
        if (!state.environment->ready()) {
            state.session->enqueue_message(
                std::move(text), std::move(attachments));
            return;
        }
        const bool generate_title     = state.session->claim_title_generation();
        const std::string title_input = text;
        state.session->clear_interrupt();
        state.session->begin_send(std::move(text), std::move(attachments));
        state.runner->spawn(
            state.session->build_history(
                full_system_prompt(state, settings.mode), settings.dialect),
            std::move(settings));
        if (generate_title && !state.runner->has_stream_override()) {
            const auto title_selection
                = state.providers->subagent_selection(SubagentRole::BASIC);
            const ProviderSelection& selected
                = title_selection ? *title_selection : *selection;
            state.delegation->spawn_title(title_input,
                make_turn_settings(selected, state.session->mode()));
        }
    }

    SessionsModal sessions_modal(const ApplicationState& state)
    {
        SessionsModal modal;
        modal.sessions = state.sessions->sessions();
        return modal;
    }

    SkillsModal skills_modal(const ApplicationState& state)
    {
        SkillsModal modal;
        const std::vector<Skill> catalog = state.environment->skills();
        const Config config              = state.providers->config();
        for (const Skill& skill : catalog) {
            const std::string root
                = skill.scope == Skill::Scope::PROJECT && skill.project_root
                ? skill.project_root->string()
                : std::string { };
            modal.entries.push_back({ skill.name, skill.description, root,
                skill_policy(config, skill) });
        }
        return modal;
    }

    void prefix_label(ModalPayload& payload, const std::string& agent_label)
    {
        if (agent_label.empty()) {
            return;
        }
        if (auto* request = std::get_if<ToolCallRequest>(&payload)) {
            request->description = agent_label + " · "
                + (request->description.empty() ? request->name
                                                : request->description);
        } else if (auto* form = std::get_if<QuestionForm>(&payload)) {
            if (!form->empty()) {
                form->front().prompt
                    = agent_label + " · " + form->front().prompt;
            }
        }
    }

    void begin_connect(ApplicationState& state, const ConnectResult& result)
    {
        state.providers->connect(result, [&state](ConnectOutcome outcome) {
            state.post([&state, outcome] {
                if (outcome.status != Status::OK) {
                    state.session->set_connect_status(
                        error_text(outcome.status));
                    return;
                }
                state.session->set_connect_status(
                    "✓ " + std::to_string(outcome.model_count) + " models");
                if (outcome.persisted
                    && std::holds_alternative<ConnectModal>(
                        state.session->modal())) {
                    state.session->set_modal(
                        ConnectModal { outcome.first_connection
                                ? ConnectModal::Entry::PICK_MODEL
                                : ConnectModal::Entry::MANAGE });
                    state.session->bump_modal_serial();
                }
            });
        });
    }

    void new_session(ApplicationState& state)
    {
        if (state.session->has_pending_work()) {
            state.session->set_error(
                "Finish or interrupt pending work before starting a new "
                "session.");
            return;
        }
        if (state.sessions->save(*state.session) != Status::OK) {
            state.session->set_error("Failed to save current session.");
            return;
        }
        // The archived file is no longer chat-active in this process.
        state.sessions->deactivate();
        state.queue.clear();
        state.skills->clear();
        state.permissions->clear();
        state.runner->clear();
        state.subagents->prune_completed();
        state.session->restore(SessionSnapshot { });
    }

    void advance_pending_skill(ApplicationState& state)
    {
        const std::optional<PendingSkillTurn> pending
            = state.skills->advance_pending_turn();
        if (!pending) {
            return;
        }
        if (pending->next < pending->awaiting.size()) {
            const Skill& skill = pending->awaiting[pending->next];
            enqueue_user_modal(state,
                evaluate_permission(
                    state, skill_request(skill), state.session->mode())
                    .request);
            return;
        }
        std::optional<PendingSkillTurn> turn
            = state.skills->take_pending_turn();
        start_turn(state, std::move(turn->text), std::move(turn->attachments));
    }

} // namespace

void submit(ApplicationState& state, std::string text,
    std::vector<FileAttachment> attachments)
{
    const std::string_view t = trim(text);
    if (t.empty()) {
        return;
    }
    if (t[0] == '/') {
        run_slash(state, t);
        return;
    }
    if (state.parent_state != nullptr
        && state.session->phase() == Session::Phase::IDLE) {
        ensure_sidechat_seeded(state);
    }
    if (state.session->phase() == Session::Phase::IDLE) {
        submit_with_skills(state, std::string(t), std::move(attachments));
    } else {
        state.session->enqueue_message(std::string(t), std::move(attachments));
    }
}

void close_modal(ApplicationState& state)
{
    if (std::holds_alternative<ToolCallRequest>(state.session->modal())) {
        interrupt(state);
    }
    resolve_modal(state, std::monostate { });
}

void enqueue_user_modal(ApplicationState& state, ModalPayload payload)
{
    state.queue.enqueue(std::move(payload), ModalOrigin::USER);
    if (state.session->modal().index() == 0
        && (state.session->phase() == Session::Phase::IDLE
            || state.session->phase() == Session::Phase::CONNECTING
            || state.session->phase() == Session::Phase::STREAMING)) {
        present_front(state);
    }
}

std::future<ModalResult> request_modal(
    ApplicationState& state, ModalPayload payload)
{
    prefix_label(payload, state.agent_label);
    if (state.parent_routing) {
        return state.parent_routing(std::move(payload));
    }

    auto promise = std::make_shared<std::promise<ModalResult>>();
    auto future  = promise->get_future();
    state.queue.enqueue(
        std::move(payload), ModalOrigin::AGENT, std::move(promise));
    state.post([&state] { present_front(state); });
    return future;
}

void present_front(ApplicationState& state)
{
    if (state.session->modal().index() != 0) {
        return;
    }
    auto pending = state.queue.peek_front();
    if (!pending) {
        return;
    }
    state.session->present_modal(std::move(pending->payload));
    if (pending->origin == ModalOrigin::AGENT) {
        notify_user(state, AgentNotification::INPUT_REQUIRED);
    }
}

void drain_queued(ApplicationState& state)
{
    std::optional<QueuedMessage> next = state.session->pop_queued();
    if (!next.has_value()) {
        return;
    }
    submit(state, std::move(next->text), std::move(next->attachments));
}

void on_turn_finished(ApplicationState& state, std::string error)
{
    const bool ended = state.session->finish_session(std::move(error));
    if (!ended) {
        return;
    }
    notify_user(state, AgentNotification::TURN_FINISHED);
    present_front(state);
    drain_queued(state);
}

void resolve_modal(ApplicationState& state, ModalResult result)
{
    bool manual_skill    = false;
    bool manual_accepted = false;
    std::optional<ToolCallRequest> manual_authorization;
    const ModalPayload current_modal = state.session->modal();
    if (const auto* request = std::get_if<ToolCallRequest>(&current_modal);
        request != nullptr && request->id == "manual-skill") {
        manual_skill        = true;
        const auto* verdict = std::get_if<ToolVerdict>(&result);
        manual_accepted
            = verdict != nullptr && verdict->decision != ToolDecision::REJECT;
        const std::optional<PendingSkillTurn> pending
            = state.skills->pending_turn();
        if (manual_accepted) {
            const PermissionEvaluation evaluation
                = evaluate_permission(state, *request, state.session->mode());
            manual_accepted
                = evaluation.decision.kind != PermissionDecision::Kind::REJECT
                && evaluation.request.args == request->args;
            if (manual_accepted) {
                manual_authorization = evaluation.request;
            }
            if (manual_accepted
                && verdict->decision == ToolDecision::ACCEPT_FOR_SESSION) {
                manual_accepted = !evaluation.session_grants.empty()
                    && state.permissions->install(evaluation.session_grants);
            }
        }
        if (manual_accepted && manual_authorization && pending
            && pending->next < pending->awaiting.size()) {
            manual_accepted = load_skill(
                state, pending->awaiting[pending->next], *manual_authorization);
        }
    }
    if (auto* path = std::get_if<std::filesystem::path>(&result)) {
        switch_session(state, *path);
    }
    if (auto* connect = std::get_if<ConnectResult>(&result)) {
        begin_connect(state, *connect);
        return;
    }
    if (auto* choice = std::get_if<ModelChoice>(&result)) {
        state.providers->select_model(*choice);
    }
    if (auto* variant = std::get_if<VariantChoice>(&result)) {
        state.providers->set_reasoning_effort(variant->effort);
        state.session->bump_modal_serial();
    }
    if (auto* skills = std::get_if<SkillPolicyChanges>(&result)) {
        if (!state.providers->set_skill_policies(*skills)) {
            state.session->set_error("Failed to save skill policies.");
            return;
        }
    }
    {
        auto entry = state.queue.try_pop();
        if (entry && entry->promise) {
            entry->promise->set_value(std::move(result));
        }
    }
    state.session->clear_modal();
    if (state.session->phase() == Session::Phase::AWAITING) {
        state.session->set_phase(Session::Phase::CONNECTING);
    }
    present_front(state);
    if (!manual_skill) {
        return;
    }
    if (!manual_accepted) {
        state.skills->take_pending_turn();
        state.session->set_error("Skill activation cancelled.");
        return;
    }
    advance_pending_skill(state);
}

void run_slash(ApplicationState& state, std::string_view command)
{
    if (state.parent_state != nullptr) {
        run_slash(*state.parent_state, command);
        return;
    }
    const SlashCommand* found = find_command(command);
    if (found == nullptr) {
        state.session->set_error(
            "Unknown command: " + std::string(command) + ".");
        return;
    }
    switch (found->action) {
    case SlashCommand::Action::EXIT: state.on_exit(); break;
    case SlashCommand::Action::NEW: new_session(state); break;
    case SlashCommand::Action::CONNECT:
        enqueue_user_modal(state, ConnectModal { ConnectModal::Entry::MANAGE });
        break;
    case SlashCommand::Action::MODEL:
        if (state.providers->connections().empty()) {
            state.session->set_error("No connections - run /connect first.");
            break;
        }
        enqueue_user_modal(
            state, ConnectModal { ConnectModal::Entry::PICK_MODEL });
        break;
    case SlashCommand::Action::VARIANT: {
        std::string current
            = to_config_effort(state.providers->status().reasoning_effort);
        enqueue_user_modal(state,
            VariantModal {
                { "off", "low", "default", "high" }, std::move(current) });
        break;
    }
    case SlashCommand::Action::SUBAGENTS:
        enqueue_user_modal(
            state, ConnectModal { ConnectModal::Entry::SUBAGENTS });
        break;
    case SlashCommand::Action::SESSIONS:
        enqueue_user_modal(state, sessions_modal(state));
        break;
    case SlashCommand::Action::SKILLS:
        enqueue_user_modal(state, skills_modal(state));
        break;
    case SlashCommand::Action::CHANGELOG: {
        const std::optional<std::string> changelog = read_changelog();
        if (!changelog) {
            state.session->set_error("Changelog file not found.");
            break;
        }
        enqueue_user_modal(
            state, ViewerModal { "Changelog", *changelog, "md", 1, false, "" });
        break;
    }
    }
}

void interrupt(ApplicationState& state)
{
    if (state.session->phase() != Session::Phase::IDLE) {
        state.session->request_interrupt();
    }
}

void switch_session(ApplicationState& state, const std::filesystem::path& path)
{
    if (state.session->has_pending_work()) {
        state.session->set_error(
            "Finish or interrupt pending work before loading a session.");
        return;
    }
    if (state.sessions->is_locked(path)) {
        state.session->set_error("Session is open in another imza process.");
        return;
    }
    if (state.sessions->save(*state.session) != Status::OK) {
        state.session->set_error("Failed to save current session.");
        return;
    }
    // Validate-then-commit: the target is read and locked before the active
    // conversation is replaced, so a failure leaves the current session
    // running; the previous lock is released only after the new one is held.
    LoadedSession loaded;
    if (read_session(path, loaded) != Status::OK) {
        state.session->set_error("Failed to load session.");
        return;
    }
    if (!state.sessions->activate(path)) {
        state.session->set_error("Session is open in another imza process.");
        return;
    }
    if (!change_directory(state, loaded.workspace)) {
        state.sessions->deactivate();
        state.session->set_error("Failed to load session.");
        return;
    }
    state.session->restore(std::move(loaded.snapshot));
    state.skills->clear();
    state.permissions->clear();
    state.runner->clear();
    state.subagents->prune_completed();
}

void delete_saved_session(
    ApplicationState& state, const std::filesystem::path& path)
{
    switch (state.sessions->remove(path)) {
    case DeleteSessionResult::INVALID_PATH:
        state.session->set_error("Invalid session path.");
        return;
    case DeleteSessionResult::REMOVE_FAILED:
        state.session->set_error("Failed to delete session.");
        return;
    case DeleteSessionResult::OK: break;
    }
    state.session->set_modal(sessions_modal(state));
    state.session->bump_modal_serial();
}

bool sidechat_open(const ApplicationState& state)
{
    return state.sidechat != nullptr && state.sidechat_open;
}

namespace {

    SessionSnapshot sidechat_seed(const ApplicationState& state)
    {
        const std::string parent_title = state.session->title();
        SessionSnapshot seed           = state.session->snapshot();
        const std::string seed_transcript
            = conversation_transcript(seed.compacted_summary, seed);
        if (!seed_transcript.empty()) {
            seed.compacted_summary = seed_transcript;
            seed.items.clear();
        }
        seed.title = "Sidechat · "
            + (parent_title.empty() ? "New Session" : parent_title);
        seed.persistence = UnsavedSession { };
        return seed;
    }

} // namespace

void ensure_sidechat_seeded(ApplicationState& state)
{
    if (state.parent_state == nullptr || state.sidechat_context_seeded) {
        return;
    }
    state.session->restore(sidechat_seed(*state.parent_state));
    state.sidechat_context_seeded = true;
}

void open_sidechat(ApplicationState& state)
{
    if (state.sidechat == nullptr) {
        state.sidechat = make_sidechat_application_state(state);
    }
    state.sidechat_open = true;
}

void close_sidechat(ApplicationState& state)
{
    if (state.sidechat == nullptr) {
        return;
    }
    interrupt(*state.sidechat);
    state.sidechat->session->clear_error();
    state.sidechat_open = false;
}

void refresh_sidechat(ApplicationState& state)
{
    if (state.sidechat == nullptr) {
        state.session->set_error(
            "No Sidechat is open - open one with Ctrl+S first.");
        return;
    }
    interrupt(*state.sidechat);
    SessionSnapshot shell = state.sidechat->session->snapshot();
    shell.items.clear();
    shell.compacted_summary.clear();
    shell.compacted_item_count = 0;
    shell.todo                 = TodoList { };
    shell.persistence          = UnsavedSession { };
    state.sidechat->session->restore(std::move(shell));
    state.sidechat->session->clear_error();
    state.sidechat->sidechat_context_seeded = false;
}

} // namespace imza
