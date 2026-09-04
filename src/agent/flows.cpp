#include "agent/flows.h"
#include "agent/prompt.h"
#include "agent/slash_commands.h"
#include "agent/turn_runner.h"
#include "common/util.h"
#include "network/json_io.h"
#include "subsystems/delegation_runner.h"
#include "subsystems/session_store.h"
#include "subsystems/skill_store.h"

#include <memory>
#include <string>
#include <utility>

namespace ursa {

namespace {

    void start_turn(ApplicationState& state, std::string text,
        std::vector<FileAttachment> attachments);

    bool load_skill(ApplicationState& state, const Skill& skill)
    {
        std::string error;
        if (!state.skills->load(skill, error)) {
            state.session->set_error(std::move(error));
            return false;
        }
        return true;
    }

    bool validate_skill_mentions(ApplicationState& state, std::string_view text)
    {
        const std::vector<Skill> catalog = state.environment->skills();
        const Config config              = state.providers->config();
        for (const std::string& name : skill_mention_names(text)) {
            Json::Value args(Json::objectValue);
            args["name"]     = name;
            const auto skill = resolve_skill(catalog, args);
            if (!skill) {
                state.session->set_error("Unknown skill: " + name + ".");
                return false;
            }
            if (skill_policy(config, *skill) == SkillPolicy::DENY) {
                state.session->set_error(
                    "Skill is denied: " + skill->name + ".");
                return false;
            }
        }
        return true;
    }

    void submit_with_skills(ApplicationState& state, std::string text,
        std::vector<FileAttachment> attachments)
    {
        if (!validate_skill_mentions(state, text)) {
            return;
        }
        const std::vector<Skill> catalog = state.environment->skills();
        std::vector<Skill> awaiting;
        for (const Skill& skill : mentioned_skills(catalog, text)) {
            if (state.skills->is_loaded(skill.path)) {
                continue;
            }
            if (skill_policy(state.providers->config(), skill)
                == SkillPolicy::ALLOW) {
                if (!load_skill(state, skill)) {
                    return;
                }
            } else {
                awaiting.push_back(skill);
            }
        }
        if (awaiting.empty()) {
            start_turn(state, std::move(text), std::move(attachments));
            return;
        }
        state.skills->set_pending_turn(PendingSkillTurn {
            std::move(text), std::move(attachments), std::move(awaiting), 0 });
        const PendingSkillTurn* pending = state.skills->pending_turn();
        const Skill& skill              = pending->awaiting.front();
        Json::Value args(Json::objectValue);
        args["name"] = skill.name;
        args["scope"]
            = skill.scope == Skill::Scope::PROJECT ? "project" : "global";
        enqueue_user_modal(state,
            ToolCallRequest { "skill", write_json(args),
                "Load skill " + skill.name, "manual-skill",
                ToolCallRequest::ApprovalReason::TOOL_PERMISSION });
    }

    void start_turn(ApplicationState& state, std::string text,
        std::vector<FileAttachment> attachments)
    {
        const std::optional<ProviderSelection> selection
            = state.providers->active_selection();
        if (!selection.has_value()) {
            state.session->set_error("No model selected — run /model.");
            return;
        }
        TurnSettings settings;
        settings.model         = selection->model;
        settings.connection_id = selection->connection_id;
        settings.reasoning_effort
            = to_config_effort(selection->reasoning_effort);
        settings.route   = selection->route;
        settings.dialect = settings.route.dialect;
        settings.mode    = state.session->mode();
        if (!state.environment->ready()) {
            state.session->enqueue_message(
                std::move(text), std::move(attachments));
            return;
        }
        const bool generate_title     = state.session->claim_title_generation();
        const std::string title_input = text;
        state.session->clear_interrupt();
        state.session->begin_send(std::move(text), std::move(attachments));
        state.runner->spawn(state.session->build_history(
                                full_system_prompt(state), settings.dialect),
            std::move(settings));
        if (generate_title && !state.runner->has_stream_override()) {
            const auto title_selection
                = state.providers->subagent_selection(SubagentRole::BASIC);
            const ProviderSelection& selected
                = title_selection ? *title_selection : *selection;
            TurnSettings title_settings;
            title_settings.model            = selected.model;
            title_settings.connection_id    = selected.connection_id;
            title_settings.reasoning_effort = selected.reasoning_effort;
            title_settings.route            = selected.route;
            title_settings.dialect          = selected.route.dialect;
            state.delegation->spawn_title(
                title_input, std::move(title_settings));
        }
    }

    SessionsModal sessions_modal(const ApplicationState& state)
    {
        SessionsModal modal;
        for (const auto& saved : saved_sessions()) {
            modal.titles.push_back(saved.title);
            modal.saved_at.push_back(saved.saved_at);
            modal.paths.push_back(saved.path.string());
        }
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
        if (save_session(*state.session) != Status::OK) {
            state.session->set_error("Failed to save current session.");
            return;
        }
        state.queue.clear();
        state.skills->clear();
        state.runner->clear();
        state.session->restore(SessionSnapshot { });
    }

    void advance_pending_skill(ApplicationState& state)
    {
        PendingSkillTurn* pending = state.skills->pending_turn();
        if (pending == nullptr) {
            return;
        }
        ++pending->next;
        if (pending->next < pending->awaiting.size()) {
            const Skill& skill = pending->awaiting[pending->next];
            Json::Value args(Json::objectValue);
            args["name"] = skill.name;
            args["scope"]
                = skill.scope == Skill::Scope::PROJECT ? "project" : "global";
            enqueue_user_modal(state,
                ToolCallRequest { "skill", write_json(args),
                    "Load skill " + skill.name, "manual-skill",
                    ToolCallRequest::ApprovalReason::TOOL_PERMISSION });
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
    if (state.session->phase() == Session::Phase::IDLE) {
        submit_with_skills(state, std::string(t), std::move(attachments));
    } else {
        state.session->enqueue_message(std::string(t), std::move(attachments));
    }
}

void close_modal(ApplicationState& state)
{
    resolve_modal(state, std::monostate { });
}

void enqueue_user_modal(ApplicationState& state, ModalPayload payload)
{
    state.queue.enqueue(std::move(payload));
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
    state.queue.enqueue(std::move(payload), promise);
    state.post([&state] { present_front(state); });
    return future;
}

void present_front(ApplicationState& state)
{
    if (state.session->modal().index() != 0) {
        return;
    }
    auto payload = state.queue.peek_front();
    if (!payload) {
        return;
    }
    state.session->present_modal(std::move(*payload));
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
    present_front(state);
    drain_queued(state);
}

void resolve_modal(ApplicationState& state, ModalResult result)
{
    bool manual_skill                = false;
    bool manual_accepted             = false;
    const ModalPayload current_modal = state.session->modal();
    if (const auto* request = std::get_if<ToolCallRequest>(&current_modal);
        request != nullptr && request->id == "manual-skill") {
        manual_skill        = true;
        const auto* verdict = std::get_if<ToolVerdict>(&result);
        manual_accepted
            = verdict != nullptr && verdict->decision != ToolDecision::REJECT;
        PendingSkillTurn* pending = state.skills->pending_turn();
        if (manual_accepted && pending != nullptr) {
            manual_accepted
                = load_skill(state, pending->awaiting[pending->next]);
        }
    }
    if (auto* path = std::get_if<std::filesystem::path>(&result)) {
        if (state.session->has_pending_work()) {
            state.session->set_error(
                "Finish or interrupt pending work before loading a session.");
            return;
        }
        if (save_session(*state.session) != Status::OK) {
            state.session->set_error("Failed to save current session.");
            return;
        }
        std::filesystem::path workspace;
        if (load_session(*path, *state.session, &workspace) != Status::OK
            || !state.environment->chdir(workspace)) {
            state.session->set_error("Failed to load session.");
        }
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
    PendingSkillTurn* pending = state.skills->pending_turn();
    if (!manual_accepted || pending == nullptr) {
        state.skills->take_pending_turn();
        state.session->set_error("Skill activation cancelled.");
        return;
    }
    advance_pending_skill(state);
}

void run_slash(ApplicationState& state, std::string_view command)
{
    run_slash_command(SlashCommandContext { state, state.on_exit,
                          [&state] { new_session(state); },
                          [&state](ModalPayload payload) {
                              enqueue_user_modal(state, std::move(payload));
                          },
                          [&state] { return sessions_modal(state); },
                          [&state] { return skills_modal(state); },
                          [&state] { return full_system_prompt(state); },
                          [&state](std::string error) {
                              state.session->set_error(std::move(error));
                          } },
        command);
}

void interrupt(ApplicationState& state)
{
    if (state.session->phase() != Session::Phase::IDLE) {
        state.session->request_interrupt();
    }
}

void delete_saved_session(
    ApplicationState& state, const std::filesystem::path& path)
{
    switch (::ursa::delete_saved_session(path)) {
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

} // namespace ursa
