#include "app/flows.h"

#include "app/slash_commands.h"
#include "common/util.h"
#include "conversation/persistence.h"
#include "permissions/evaluator.h"
#include "permissions/permission_store.h"
#include "permissions/shell_analysis.h"
#include "platform/command_runner.h"
#include "platform/config.h"
#include "platform/credential_subsystem.h"
#include "runtime/mcp_subsystem.h"
#include "tools/file_ops.h"
#include "tools/skills.h"
#include "turn/delegation.h"
#include "turn/prompt_store.h"
#include "turn/turn_runner.h"
#include "workspace/review_subsystem.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace imza {

namespace {
    // Terminal alert (OSC 9 / bell) for an event the user may be away
    // from. Only the root attended session alerts: children and the
    // sidechat share the store, and headless installs no terminal
    // subscriber, so the alert is a no-op there.
    void notify_user(ApplicationState& state, AgentNotification notification,
        std::optional<std::string> message = std::nullopt)
    {
        if (!state.agent_label.empty()
            || (state.runtime_flags & RuntimeFlag::ATTENDED)
                == RuntimeFlag::NONE
            || !state.notifications) {
            return;
        }
        if (message.has_value()) {
            state.notifications->alert(std::move(*message));
            return;
        }
        state.notifications->alert(
            notification == AgentNotification::TURN_FINISHED
                ? "Imza finished"
                : "Imza needs your attention");
    }

    void notify_review_pass(
        ApplicationState& state, ReviewPass pass, bool success)
    {
        notify_user(state, AgentNotification::REVIEW_PASS_FINISHED,
            pass == ReviewPass::HANDOFF
                ? (success ? "Handoff report ready" : "Handoff report failed")
                : (success ? "AI review finished" : "AI review failed"));
    }

    Environment::ChdirResult change_directory(
        ApplicationState& state, const std::filesystem::path& directory)
    {
        std::error_code error;
        const std::filesystem::path canonical
            = std::filesystem::weakly_canonical(directory, error);
        if (error) {
            return Environment::ChdirResult::FAILED;
        }
        const Environment::ChdirResult result
            = state.environment->chdir(canonical);
        if (result != Environment::ChdirResult::CHANGED) {
            return result;
        }
        // Grants and loaded skills are workspace-scoped: neither crosses
        // into the new directory.
        state.permissions->clear();
        state.session->clear_loaded_skills();
        return result;
    }

    void start_turn(ApplicationState& state, std::string text,
        std::vector<Attachment> attachments);

    void advance_pending_skill(ApplicationState& state);

    bool load_skill(ApplicationState& state, const Skill& skill,
        const ToolCallRequest& authorized)
    {
        if (!authorized_skill_path(skill, authorized)) {
            state.session->set_error(
                "Skill permission target changed before activation.");
            return false;
        }
        std::string error;
        if (!state.session->load_skill(skill, error)) {
            state.session->set_error(std::move(error));
            return false;
        }
        return true;
    }

    ToolCallRequest skill_request(const Skill& skill)
    {
        SkillToolArgs args;
        args.name = skill.name;
        args.scope
            = skill.scope == Skill::Scope::PROJECT ? "project" : "global";
        return { "skill", json_dump(args), "Load skill " + skill.name,
            "manual-skill" };
    }

    // Rebuilds the normalized call a prompt approved, for re-evaluation.
    ToolCallRequest authorized_request(const PermissionPrompt& prompt)
    {
        const auto& details = std::get<SkillRequest>(prompt.request);
        SkillToolArgs args;
        args.name  = details.name;
        args.scope = details.scope;
        args.path  = details.path;
        return { "skill", json_dump(args), prompt.description, prompt.id };
    }

    void submit_with_skills(ApplicationState& state, std::string text,
        std::vector<Attachment> attachments)
    {
        const std::vector<Skill> catalog = state.environment->skills();
        std::vector<Skill> awaiting;
        for (const Skill& skill : mentioned_skills(catalog, text)) {
            const PermissionEvaluation evaluation = evaluate_permission(
                state, skill_request(skill), state.session->mode());
            if (evaluation.decision.kind == PermissionDecision::Kind::ACCEPT
                || state.permissions->skip_permissions()) {
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
        state.session->set_pending_skill_turn(PendingSkillTurn {
            std::move(text), std::move(attachments), std::move(awaiting), 0 });
        const PermissionEvaluation evaluation = evaluate_permission(
            state, skill_request(first), state.session->mode());
        if (evaluation.prompt) {
            enqueue_user_modal(state, *evaluation.prompt);
            return;
        }
        // No prompt: the skill was denied or is already authorized;
        // skip it and prompt for the next awaiting skill or start the turn.
        advance_pending_skill(state);
    }

    // Shared launch path for submit- and slash-initiated turns. Applies
    // `mode` when it differs from the session's, so the make-skill flow can
    // switch into Build only after the model guard passes.
    void begin_turn(ApplicationState& state, Session::Mode mode,
        std::string text, std::vector<Attachment> attachments,
        std::string title, std::optional<Message> extra)
    {
        const std::optional<ProviderSelection> selection
            = state.providers->active_selection();
        if (!selection.has_value()) {
            state.session->set_error(NO_MODEL_SELECTED);
            return;
        }
        if (mode != state.session->mode()) {
            state.session->set_mode(mode);
        }
        if (!state.environment->ready()) {
            state.session->enqueue_message(
                std::move(text), std::move(attachments));
            return;
        }
        // Starting before the worker lands would send an unauthenticated
        // request. Bounded, so a wedged keyring surfaces as a turn error.
        if (!state.credentials->wait_ready(std::chrono::seconds { 10 })) {
            state.session->set_error(
                "Credentials are still loading; try again in a moment.");
            return;
        }
        const TurnSettings settings = make_turn_settings(*selection, mode);
        const bool generate_title   = state.session->claim_title_generation();
        state.session->clear_interrupt();
        state.session->begin_send(std::move(text), std::move(attachments));
        std::optional<std::string> submission
            = state.session->plan_submission_for_build();
        std::vector<Message> history = state.session->build_history(
            full_system_prompt(*state.prompts, *state.environment,
                *state.providers, state.mcp.get(), state.session.get(),
                settings.mode),
            settings.route.dialect);
        if (submission) {
            history.push_back({ Message::Type::USER, std::move(*submission) });
        }
        if (extra) {
            history.push_back(std::move(*extra));
        }
        state.runner->spawn(std::move(history), std::move(settings));
        if (generate_title && !state.runner->has_stream_override()) {
            const auto title_selection
                = state.providers->subagent_selection(SubagentRole::BASIC);
            const ProviderSelection& selected
                = title_selection ? *title_selection : *selection;
            state.delegation->spawn_title(std::move(title),
                make_turn_settings(selected, state.session->mode()));
        }
    }

    void start_turn(ApplicationState& state, std::string text,
        std::vector<Attachment> attachments)
    {
        begin_turn(state, state.session->mode(), text, std::move(attachments),
            text, std::nullopt);
    }

    SessionsModal sessions_modal() { return { }; }

    void prefix_label(ModalPayload& payload, const std::string& agent_label)
    {
        if (agent_label.empty()) {
            return;
        }
        if (auto* prompt = std::get_if<PermissionPrompt>(&payload)) {
            prompt->description = agent_label + " · "
                + (prompt->description.empty() ? prompt->name
                                               : prompt->description);
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

    bool save_active_session(ApplicationState& state)
    {
        if (state.sessions->save(*state.session) != Status::OK) {
            state.session->set_error("Failed to save current session.");
            return false;
        }
        return true;
    }

    void install_session(ApplicationState& state, SessionSnapshot snapshot)
    {
        state.session->restore(std::move(snapshot));
        // Grants are workspace-scoped: a new conversation starts clean.
        state.permissions->clear();
        state.runner->clear();
        state.subagents->prune_completed();
    }

    void new_session(ApplicationState& state)
    {
        if (state.session->has_pending_work()) {
            state.session->set_error(
                "Finish or interrupt pending work before starting a new "
                "session.");
            return;
        }
        if (!save_active_session(state)) {
            return;
        }
        // The archived file is no longer chat-active in this process.
        state.sessions->deactivate();
        install_session(state, SessionSnapshot { });
    }

    void advance_pending_skill(ApplicationState& state)
    {
        const std::optional<PendingSkillTurn> pending
            = state.session->advance_pending_skill_turn();
        if (!pending) {
            return;
        }
        if (pending->next < pending->awaiting.size()) {
            const Skill& skill = pending->awaiting[pending->next];
            const PermissionEvaluation evaluation = evaluate_permission(
                state, skill_request(skill), state.session->mode());
            // An earlier approval can grant a later queued skill too;
            // with no prompt left, advance to the next one.
            if (evaluation.prompt) {
                enqueue_user_modal(state, *evaluation.prompt);
                return;
            }
            advance_pending_skill(state);
            return;
        }
        std::optional<PendingSkillTurn> turn
            = state.session->take_pending_skill_turn();
        start_turn(state, std::move(turn->text), std::move(turn->attachments));
    }

} // namespace

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
            skill_policy(config, skill), skill.bundled });
    }
    return modal;
}

void submit(ApplicationState& state, std::string text,
    std::vector<Attachment> attachments)
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
    if (std::holds_alternative<PermissionPrompt>(state.session->modal())) {
        interrupt(state);
    }
    resolve_modal(state, std::monostate { });
}

void enqueue_user_modal(ApplicationState& state, ModalPayload payload)
{
    state.session->queue.enqueue(std::move(payload), ModalOrigin::USER);
    if (state.session->modal().index() == 0
        && (state.session->phase() == Session::Phase::IDLE
            || state.session->phase() == Session::Phase::CONNECTING
            || state.session->phase() == Session::Phase::STREAMING)) {
        present_front(state);
    }
}

namespace {

    // Applies `mutate` to the persisted config through the one shared store;
    // on a committed change the MCP manager reloads the resulting server map.
    // False when nothing was written.
    bool mutate_mcp_config(
        ApplicationState& state, const std::function<bool(Config&)>& mutate)
    {
        if (state.config->update(mutate) != ConfigUpdateResult::UPDATED) {
            return false;
        }
        if (state.mcp) {
            state.mcp->reload(state.config->snapshot()->mcp_servers);
        }
        return true;
    }

} // namespace

void mcp_add_server(ApplicationState& state, const McpServerConfig& server)
{
    if (!mutate_mcp_config(state, [&server](Config& cfg) {
            return cfg.mcp_servers.insert_or_assign(server.id, server).second;
        })) {
        return;
    }
    if (state.mcp) {
        state.mcp->connect(server.id);
    }
}

bool mcp_remove_server(ApplicationState& state, const std::string& id)
{
    return mutate_mcp_config(
        state, [&id](Config& cfg) { return cfg.mcp_servers.erase(id) != 0; });
}

bool mcp_set_server_autoload(
    ApplicationState& state, const std::string& id, bool autoload)
{
    return mutate_mcp_config(state, [id, autoload](Config& cfg) {
        const auto found = cfg.mcp_servers.find(id);
        if (found == cfg.mcp_servers.end()) {
            return false;
        }
        found->second.autoload = autoload;
        return true;
    });
}

bool mcp_begin_server_sign_in(
    ApplicationState& state, const McpServerConfig& server)
{
    // Added disabled: connect() no-ops until the flow enables the entry,
    // and a cancelled first sign-in leaves a removable, retryable row.
    if (!mutate_mcp_config(state, [&server](Config& cfg) {
            return cfg.mcp_servers.insert_or_assign(server.id, server).second;
        })) {
        return false;
    }
    if (state.mcp) {
        state.mcp->sign_in(server.id);
    }
    return true;
}

// The persisted allow list, ordered directories first so the modal's row
// index maps to one section deterministically.
std::vector<PermissionGrant> allow_grants_view(const ApplicationState& state)
{
    std::vector<PermissionGrant> out;
    const auto config = state.config->snapshot();
    for (const std::filesystem::path& directory : config->allow.directories) {
        out.emplace_back(directory);
    }
    for (const ShellCommandGrant& command : config->allow.commands) {
        out.emplace_back(command);
    }
    return out;
}

bool allow_add_directory(
    ApplicationState& state, const std::string& path, std::string& error)
{
    const std::string trimmed(trim(path));
    if (trimmed.empty()) {
        error = "Enter a directory to allow.";
        return false;
    }
    std::filesystem::path directory = path_from_utf8(expand_user_path(trimmed));
    if (directory.is_relative()) {
        // Matches the config file's rule: relative entries resolve against
        // the directory Imza starts in.
        directory
            = state.environment->workspace()->working_directory / directory;
    }
    if (!std::filesystem::is_directory(directory)) {
        error = "Not a directory: " + trimmed;
        return false;
    }
    std::error_code fs_error;
    const std::filesystem::path canonical
        = std::filesystem::weakly_canonical(directory, fs_error);
    if (fs_error) {
        error = "Cannot resolve: " + trimmed;
        return false;
    }
    const std::vector<PermissionGrant> existing = allow_grants_view(state);
    for (const PermissionGrant& grant : existing) {
        if (grant_covers(grant, ExternalGrant { canonical })) {
            error = "Already covered by an existing grant.";
            return false;
        }
    }
    if (state.config->update([&canonical](Config& cfg) {
            for (const std::filesystem::path& stored : cfg.allow.directories) {
                if (stored == canonical) {
                    return false;
                }
            }
            cfg.allow.directories.push_back(canonical);
            return true;
        })
        != ConfigUpdateResult::UPDATED) {
        error = "Could not save the configuration.";
        return false;
    }
    state.permissions->install({ ExternalGrant { canonical } });
    return true;
}

bool allow_add_command(
    ApplicationState& state, const std::string& command, std::string& error)
{
    const std::string trimmed(trim(command));
    if (trimmed.empty()) {
        error = "Enter a command to allow.";
        return false;
    }
    // Same gate as --allow-cmd: only reusable, classifiable invocations
    // may become persistent grants, so the two entry points never
    // disagree on what is grantable.
    const ShellAnalysis analysis = analyze_shell(trimmed);
    if (analysis.reuse != ShellAnalysis::Reuse::SESSION
        || analysis.invocations.size() != 1) {
        error = "invalid --allow-cmd value: '" + trimmed + "'";
        return false;
    }
    const ShellInvocation& invocation = analysis.invocations.front();
    if (shell_deletion_denied(invocation.program)) {
        error = "cannot allow '" + invocation.program
            + "': deletion runs through imza.fs.remove";
        return false;
    }
    if (shell_launcher_denied(invocation.program)) {
        error = "cannot allow '" + invocation.program
            + "': it runs code that cannot be inspected, so a grant would "
              "authorize every command it can run";
        return false;
    }
    const ShellCommandGrant grant { invocation.program, invocation.subcommand };
    const std::vector<PermissionGrant> existing = allow_grants_view(state);
    for (const PermissionGrant& stored : existing) {
        if (grant_covers(stored, grant)) {
            error = "Already covered by an existing grant.";
            return false;
        }
    }
    if (state.config->update([&grant](Config& cfg) {
            for (const ShellCommandGrant& stored : cfg.allow.commands) {
                if (stored == grant) {
                    return false;
                }
            }
            cfg.allow.commands.push_back(grant);
            return true;
        })
        != ConfigUpdateResult::UPDATED) {
        error = "Could not save the configuration.";
        return false;
    }
    state.permissions->install({ grant });
    return true;
}

bool allow_remove_directory(ApplicationState& state, std::size_t index)
{
    const bool removed = state.config->update([index](Config& cfg) {
        if (index >= cfg.allow.directories.size()) {
            return false;
        }
        cfg.allow.directories.erase(
            cfg.allow.directories.begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }) == ConfigUpdateResult::UPDATED;
    if (!removed) {
        state.session->set_error("Failed to save permission grants.");
        return false;
    }
    return true;
}

bool allow_remove_command(ApplicationState& state, std::size_t index)
{
    const bool removed = state.config->update([index](Config& cfg) {
        if (index >= cfg.allow.commands.size()) {
            return false;
        }
        cfg.allow.commands.erase(
            cfg.allow.commands.begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }) == ConfigUpdateResult::UPDATED;
    if (!removed) {
        state.session->set_error("Failed to save permission grants.");
        return false;
    }
    return true;
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
    state.session->queue.enqueue(
        std::move(payload), ModalOrigin::AGENT, std::move(promise));
    state.post([&state] { present_front(state); });
    return future;
}

void present_front(ApplicationState& state)
{
    if (state.session->modal().index() != 0) {
        return;
    }
    auto pending = state.session->queue.peek_front();
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
    const bool failed = !error.empty();
    const bool ended  = state.session->finish_session(std::move(error));
    if (!ended) {
        return;
    }
    // The final turn error bypasses set_error, so publish it here.
    if (failed && state.notifications) {
        state.notifications->publish(true, state.session->error());
    }
    notify_user(state, AgentNotification::TURN_FINISHED);
    present_front(state);
    drain_queued(state);
}

PermissionContext permission_context_for(
    ApplicationState& state, Session::Mode mode)
{
    return make_permission_context(*state.environment, *state.permissions, mode,
        state.session->loaded_skill_directories(), state.mcp.get());
}

PermissionEvaluation evaluate_permission(
    ApplicationState& state, const ToolCallRequest& request, Session::Mode mode)
{
    return evaluate_tool_request(request, permission_context_for(state, mode),
        state.providers->config(), state.environment->skills(), *state.session);
}

void resolve_modal(ApplicationState& state, ModalResult result)
{
    bool manual_skill                = false;
    bool manual_accepted             = false;
    const ModalPayload current_modal = state.session->modal();
    if (const auto* prompt = std::get_if<PermissionPrompt>(&current_modal);
        prompt != nullptr && prompt->id == "manual-skill") {
        manual_skill        = true;
        const auto* verdict = std::get_if<ToolVerdict>(&result);
        manual_accepted
            = verdict != nullptr && verdict->decision != ToolDecision::REJECT;
        const std::optional<PendingSkillTurn> pending
            = state.session->pending_skill_turn();
        std::optional<ToolCallRequest> manual_authorization;
        if (manual_accepted) {
            const ToolCallRequest request = authorized_request(*prompt);
            const PermissionEvaluation evaluation
                = evaluate_permission(state, request, state.session->mode());
            manual_accepted
                = evaluation.decision.kind != PermissionDecision::Kind::REJECT
                && evaluation.request.args == request.args;
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
        // Unlike other results the connect modal stays open: begin_connect
        // morphs it to PICK_MODEL/MANAGE once the async connect completes, so
        // the entry must not be popped or cleared here.
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
    {
        auto entry = state.session->queue.try_pop();
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
        state.session->take_pending_skill_turn();
        state.session->set_error("Skill activation cancelled.");
        return;
    }
    advance_pending_skill(state);
}

// Forced manual compaction on the runner's compaction worker; the session
// is IDLE and stays that way - the compaction event shows progress.
void compact_session(ApplicationState& state, TurnSettings settings)
{
    if (state.runner->compaction_active()) {
        state.session->set_error("Compaction already in progress.");
        return;
    }
    state.session->clear_interrupt();
    state.runner->spawn_compaction(std::move(settings));
}

namespace {

    // Seeded Build turn: flips the session to Build. The visible transcript
    // records only the command line; the seeded instructions ride the model
    // history as an extra user message, like the plan submission.
    void start_make_skill_turn(ApplicationState& state, std::string description)
    {
        begin_turn(state, Session::Mode::BUILD,
            "/make-skill " + std::string(trim(description)), { },
            "/make-skill " + description,
            Message { Message::Type::USER,
                state.prompts->make_skill() + description });
    }

} // namespace

void run_slash(ApplicationState& state, std::string_view command)
{
    if (state.parent_state != nullptr) {
        run_slash(*state.parent_state, command);
        return;
    }
    std::string_view args;
    std::string_view base = command;
    if (const std::size_t space = command.find(' ');
        space != std::string_view::npos) {
        base = command.substr(0, space);
        args = trim(command.substr(space + 1));
    }
    const SlashCommand* found = find_command(base);
    if (found == nullptr) {
        std::string message = "Unknown command: " + std::string(base) + ".";
        if (const auto suggestion = suggest_command(base)) {
            message
                += " Did you mean " + std::string(suggestion->command->name);
            if (!suggestion->alias.empty()) {
                message += " (" + std::string(suggestion->alias) + ")";
            }
            message += "?";
        }
        state.session->set_error(std::move(message));
        return;
    }
    switch (found->action) {
    case SlashCommand::Action::EXIT: state.on_exit(); break;
    case SlashCommand::Action::NEW: new_session(state); break;
    case SlashCommand::Action::CONNECT:
        enqueue_user_modal(state, ConnectModal { ConnectModal::Entry::MANAGE });
        break;
    case SlashCommand::Action::MCP:
        enqueue_user_modal(state, McpModal { });
        break;
    case SlashCommand::Action::PERMISSIONS:
        enqueue_user_modal(state, PermissionsModal { });
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
        const std::vector<std::string> choices(
            REASONING_EFFORT_CHOICES.begin(), REASONING_EFFORT_CHOICES.end());
        std::string current
            = to_config_effort(state.providers->status().reasoning_effort);
        enqueue_user_modal(state, VariantModal { choices, std::move(current) });
        break;
    }
    case SlashCommand::Action::SUBAGENTS:
        enqueue_user_modal(
            state, ConnectModal { ConnectModal::Entry::SUBAGENTS });
        break;
    case SlashCommand::Action::SESSIONS:
        enqueue_user_modal(state, sessions_modal());
        break;
    case SlashCommand::Action::SKILLS:
        enqueue_user_modal(state, skills_modal(state));
        break;
    case SlashCommand::Action::COMPACT:
        if (state.session->has_pending_work()) {
            state.session->set_error(
                "Finish or interrupt pending work before compacting.");
            break;
        }
        if (!state.session->has_items()) {
            state.session->set_error("Nothing to compact.");
            break;
        }
        if (const auto selection = state.providers->active_selection();
            !selection.has_value()) {
            state.session->set_error(NO_MODEL_SELECTED);
            break;
        }
        compact_session(state,
            make_turn_settings(
                *state.providers->active_selection(), state.session->mode()));
        break;
    case SlashCommand::Action::MAKE_SKILL:
        if (args.empty()) {
            state.session->set_error(
                "Usage: /make-skill <workflow description>");
            break;
        }
        if (args.size() > 2000) {
            args = args.substr(0, 2000);
        }
        start_make_skill_turn(state, std::string(args));
        break;
    case SlashCommand::Action::AUTO: {
        const std::string arg = to_lower(args);
        if (!arg.empty() && arg != "on" && arg != "off") {
            state.session->set_error("Usage: /auto on|off");
            break;
        }
        const bool on = arg.empty() ? !state.permissions->skip_permissions()
                                    : arg == "on";
        state.permissions->set_skip_permissions(on);
        break;
    }
    case SlashCommand::Action::CHDIR: {
        if (args.empty()) {
            state.session->set_error("Usage: /cd <directory>");
            break;
        }
        // A busy session queues the command; drain_queued re-runs it once
        // idle. Phase-only: enqueueing on a non-empty queue would
        // re-enqueue the popped command forever.
        if (state.session->phase() != Session::Phase::IDLE) {
            state.session->enqueue_message(std::string(command));
            break;
        }
        const std::filesystem::path target
            = path_from_utf8(expand_user_path(args));
        std::error_code error;
        if (!std::filesystem::is_directory(target, error) || error) {
            state.session->set_error("Not a directory: " + std::string(args));
            break;
        }
        switch (change_directory(state, target)) {
        case Environment::ChdirResult::FAILED:
            state.session->set_error("Cannot change to: " + std::string(args));
            break;
        case Environment::ChdirResult::UNCHANGED: break;
        case Environment::ChdirResult::CHANGED: {
            const auto workspace = state.environment->workspace();
            state.session->append_notice("Working directory changed to "
                + (workspace ? workspace->working_directory.string()
                             : std::string(args)));
            break;
        }
        }
        break;
    }
    case SlashCommand::Action::CHANGELOG: {
        const std::optional<std::string> changelog = read_changelog();
        if (!changelog) {
            state.session->set_error("Changelog file not found.");
            break;
        }
        enqueue_user_modal(state,
            ViewerModal { .title = "Changelog",
                .content         = *changelog,
                .lang            = "md",
                .line_numbers    = false });
        break;
    }
    }
}

namespace {

    // Review-pass budget: the input cap comes from the model's context
    // window, falling back to the byte cap when the window is unknown. The
    // pass sends no output cap, matching the main chat: a fixed cap cut
    // valid replies off at the token limit.
    constexpr std::size_t FALLBACK_REVIEW_PASS_PROMPT_BYTES = 200 * 1024;
    constexpr std::uint64_t MIN_REVIEW_CONTEXT_LIMIT        = 16384;
    // Bytes-per-token over-provision: the guard rejects early, the model
    // is the true limit.
    constexpr std::size_t BYTES_PER_TOKEN = 3;
    // Pass deadline. The transport's own 5-minute stall window bounds a
    // dead stream, so this only needs to cover a slow model; it must not
    // be shorter than that or a working pass is cut off mid-flight.
    constexpr std::chrono::seconds REVIEW_PASS_TIMEOUT { 300 };

    std::size_t review_input_limit(
        const ProviderStore& providers, const ProviderSelection& selection)
    {
        const std::uint64_t window = providers.context_limit_for(
            selection.model, selection.connection_id);
        if (window < MIN_REVIEW_CONTEXT_LIMIT) {
            return FALLBACK_REVIEW_PASS_PROMPT_BYTES;
        }
        // Half the window for input, the rest for output and overhead.
        return static_cast<std::size_t>(window / 2 * BYTES_PER_TOKEN);
    }

    // A filename-safe suffix of a viewer title: lowercase, runs of
    // non-alphanumerics collapsed to one dash, bounded; "" when the title
    // has no alphanumerics.
    std::string export_slug(std::string_view title)
    {
        std::string slug;
        bool pending_dash = false;
        for (const unsigned char ch : title) {
            if (!std::isalnum(ch)) {
                pending_dash = !slug.empty();
                continue;
            }
            if (pending_dash) {
                slug += '-';
                pending_dash = false;
            }
            slug += static_cast<char>(std::tolower(ch));
            if (slug.size() >= 60) {
                break;
            }
        }
        return slug;
    }

    // The platform file opener, not VISUAL/EDITOR: the viewer runs
    // inside the TUI, so a terminal editor (vim, nano) cannot take over
    // the screen. xdg-open / open -t / notepad open their own window and
    // leave imza's terminal untouched.
    std::string editor_command(const std::filesystem::path& path)
    {
#if defined(_WIN32)
        return "cmd /c start \"\" " + shell_quote(path);
#elif defined(__APPLE__)
        return "open -t " + shell_quote(path);
#else
        return "xdg-open " + shell_quote(path);
#endif
    }

} // namespace

std::optional<std::size_t> run_review_pass(
    ApplicationState& state, ReviewPassRequest request, std::string& error)
{
    const ReviewSubsystem::Snapshot snapshot = state.review->snapshot();
    // Each pass owns its own slot and Cancel button, so the two may run
    // together; only a repeat of the same pass is refused.
    const bool running = request.pass == ReviewPass::HANDOFF
        ? snapshot.handoff.task_id.has_value()
        : snapshot.findings.task_id.has_value();
    if (running) {
        error = request.label + " is already running.";
        return std::nullopt;
    }
    if (snapshot.status != ReviewSubsystem::LoadStatus::LOADED
        || !snapshot.review || snapshot.review->files.empty()) {
        error = "There are no changes to review.";
        return std::nullopt;
    }
    // The one-shot Basic role, shared with session titles; <Default>
    // follows the main chat model.
    auto selection = state.providers->subagent_selection(SubagentRole::BASIC);
    if (!selection) {
        selection = state.providers->active_selection();
    }
    if (!selection) {
        error = NO_MODEL_SELECTED;
        return std::nullopt;
    }
    const std::size_t input_limit
        = review_input_limit(*state.providers, *selection);
    if (request.prompt.size() > input_limit) {
        error = request.label
            + " is too large (limit: " + std::to_string(input_limit / 1024)
            + " KiB). Reduce the diff or review it in smaller commits.";
        return std::nullopt;
    }
    auto transcript = std::make_shared<Session>();
    // Kept for the one-shot retry below; the pass itself consumes a copy.
    std::string retry_prompt = request.prompt;
    // Extraction, not open reasoning: hidden thinking tokens buy nothing
    // and can consume the whole deadline before any output appears.
    const std::string effort    = "off";
    const SubagentHandle handle = state.delegation->run_subagent(
        std::move(request.prompt), selection->model, effort,
        SubagentOptions { .visible = false,
            .bare_system           = true,
            .timeout               = REVIEW_PASS_TIMEOUT,
            .transcript            = std::move(transcript) },
        [state_ptr = &state, pass = request.pass, label = request.label,
            prompt = std::move(retry_prompt), retry = request.retry,
            complete = std::move(request.complete)](
            const SubagentResult& result) mutable {
            // Clear through the queue, not directly: this callback runs on
            // the finishing worker, which can complete before
            // run_review_pass stores the task id below. Posting the clear
            // makes it land after that store, so a fast pass cannot leave
            // a stale id behind and block its own slot.
            state_ptr->post([state_ptr, pass] {
                state_ptr->review->set_pass_task(pass, std::nullopt);
            });
            if (result.status == Status::CANCELLED) {
                return;
            }
            // A truncated reply is the one failure worth retrying: the
            // JSON envelope is cut off, so it parses as malformed even
            // though the model was on track. Any other non-OK status is
            // terminal.
            const bool truncated = result.status == Status::TRUNCATED;
            if (result.status != Status::OK && !truncated) {
                state_ptr->session->set_error(
                    label + " failed: " + error_text(result.status));
                notify_review_pass(*state_ptr, pass, false);
                return;
            }
            std::string parse_error;
            if (!truncated) {
                parse_error = complete(result.output);
                if (parse_error.empty()) {
                    notify_review_pass(*state_ptr, pass, true);
                    return;
                }
            }
            if (!retry) {
                state_ptr->session->set_error(
                    truncated ? error_text(Status::TRUNCATED) : parse_error);
                notify_review_pass(*state_ptr, pass, false);
                return;
            }
            // One deterministic rerun without reasoning recovers both a
            // truncated reply and a shape failure the budget caused. It
            // must start on the main thread: this callback runs on the
            // finishing subagent's own worker, and run_review_pass prunes
            // completed workers, joining the thread it is running on.
            const std::string failure_message
                = truncated ? error_text(Status::TRUNCATED) : parse_error;
            state_ptr->post([state_ptr, pass, label, prompt,
                                complete = std::move(complete),
                                failure_message]() mutable {
                std::string retry_error;
                const std::optional<std::size_t> rerun
                    = run_review_pass(*state_ptr,
                        ReviewPassRequest { .pass = pass,
                            .prompt               = prompt,
                            .label                = label,
                            .complete             = std::move(complete),
                            .retry                = false },
                        retry_error);
                if (!rerun) {
                    state_ptr->session->set_error(
                        retry_error.empty() ? failure_message : retry_error);
                    notify_review_pass(*state_ptr, pass, false);
                }
            });
        });
    state.review->set_pass_task(request.pass, handle.id);
    return handle.id;
}

bool cancel_review_pass(ApplicationState& state, ReviewPass pass)
{
    const ReviewSubsystem::Snapshot snapshot = state.review->snapshot();
    const ReviewPassState& current
        = pass == ReviewPass::HANDOFF ? snapshot.handoff : snapshot.findings;
    if (!current.task_id) {
        return false;
    }
    return state.subagents->cancel(*current.task_id);
}

void run_handoff(ApplicationState& state)
{
    const ReviewSubsystem::Snapshot snapshot = state.review->snapshot();
    if (!snapshot.review) {
        state.session->set_error("There are no changes to report on.");
        return;
    }
    const SessionSnapshot session = state.session->snapshot();
    const std::string prompt      = format_handoff_prompt(
        state.prompts->handoff(), session, *snapshot.review);
    std::string error;
    (void)run_review_pass(state,
        ReviewPassRequest { .pass = ReviewPass::HANDOFF,
            .prompt               = std::move(prompt),
            .label                = "Handoff report",
            .complete
            = [state_ptr = &state](std::string output) -> std::string {
                std::string report { trim(std::move(output)) };
                if (report.empty()) {
                    // An empty reply is a shape failure worth one retry.
                    return std::string("The handoff document came back "
                                       "empty.");
                }
                report += "\n";
                state_ptr->session->append_handoff_report(std::move(report));
                if (state_ptr->notifications) {
                    state_ptr->notifications->publish(
                        false, "Handoff report saved.");
                }
                return std::string { };
            } },
        error);
    if (!error.empty()) {
        state.session->set_error(std::move(error));
    }
}

std::string export_viewer_content(
    ApplicationState& state, const ViewerModal& payload, std::string& error)
{
    const auto workspace = state.environment->workspace();
    if (!workspace) {
        error = "The workspace is not ready.";
        return { };
    }
    const std::filesystem::path root
        = workspace->project_root.value_or(workspace->working_directory);
    const std::string name = format_local_time("%Y%m%d-%H%M%S") + "-"
        + export_slug(
            payload.save_name.empty() ? payload.title : payload.save_name);
    const std::string path = (root / ".imza" / (name + ".md")).string();
    const WriteFileRequest request { path, payload.content };
    // User-initiated and always inside the trusted root, so it is
    // evaluated as a Build-mode write: the Plan-mode ban guards agent
    // mutation, not an explicit export.
    const FilesystemEvaluation evaluation = evaluate_filesystem_request(
        request, permission_context_for(state, SessionMode::BUILD));
    if (evaluation.decision.kind != PermissionDecision::Kind::ACCEPT) {
        error = "Cannot export: " + evaluation.decision.reason;
        return { };
    }
    std::string write_error;
    if (!save_text(filesystem_target(request).string(), payload.content,
            write_error)) {
        error = "Could not export: " + write_error;
        return { };
    }
    if (!spawn_detached_command(editor_command(path))) {
        error = "Exported to " + path + " but the editor could not start.";
        return { };
    }
    return path;
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
    if (!save_active_session(state)) {
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
    if (change_directory(state, loaded.workspace)
        == Environment::ChdirResult::FAILED) {
        state.sessions->deactivate();
        state.session->set_error("Failed to load session.");
        return;
    }
    install_session(state, std::move(loaded.snapshot));
}

bool restore_session_at_startup(ApplicationState& state,
    const std::filesystem::path& path,
    std::optional<std::filesystem::path> workspace_override)
{
    if (state.sessions->is_locked(path)) {
        return false;
    }
    LoadedSession loaded;
    if (read_session(path, loaded) != Status::OK
        || !state.sessions->activate(path)) {
        return false;
    }
    state.session->restore(std::move(loaded.snapshot));
    const std::filesystem::path workspace
        = workspace_override ? *workspace_override : loaded.workspace;
    if (state.environment->chdir(workspace)
        == Environment::ChdirResult::FAILED) {
        state.sessions->deactivate();
        return false;
    }
    return true;
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
    state.session->set_modal(sessions_modal());
    state.session->bump_modal_serial();
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
            // The transcript absorbed everything the parent had marked
            // compacted; nothing is skipped until the sidechat compacts.
            seed.compacted_item_count = 0;
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
            "No Sidechat is open - open one with Ctrl+E first.");
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
