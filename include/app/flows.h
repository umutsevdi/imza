#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/application_state.h"
#include "common/modal.h"
#include "permissions/store.h"
#include "turn/turn_runner.h"
#include "workspace/review.h"

namespace imza {

class MainThreadQueue;

struct OneShotRequest {
    enum class Mode { ASK, EXEC };
    Mode mode;
    std::string query;
};

struct OneShotResult {
    enum class Kind {
        SUCCESS,
        BLOCKED_PERMISSION,
        INTERRUPTED,
        PROVIDER_FAILURE,
        TOOL_FAILURE
    };
    Kind kind = Kind::SUCCESS;
    std::string output;
    std::string error;
};

struct CliResult {
    bool continue_as_interactive = true;
    int exit_code                = 0;
    std::optional<std::filesystem::path> working_directory;
    std::optional<std::filesystem::path> session_path;
    std::optional<std::string> model;
    std::optional<std::string> variant;
    std::optional<bool> web;
    std::optional<bool> shell;
    std::optional<OneShotRequest> one_shot;
    std::vector<std::filesystem::path> allowed_directories;
    std::vector<ShellCommandGrant> allowed_commands;
    // Ids and names, not grants: main.cpp resolves them once the manager
    // and skill catalogue exist, and fails startup on an unknown one.
    std::vector<std::string> allowed_mcp;
    std::vector<std::string> allowed_skills;
    bool skip_permissions = false;
};

CliResult run_cli(int argc, char** argv);
// One context for every agent-facing gate: grants, mode, skill
// directories, MCP inventory.
PermissionContext permission_context_for(
    ApplicationState& state, Session::Mode mode);
std::string format_session_list(const std::vector<SavedSession>& sessions);
RuntimeFlag runtime_flags_for(const CliResult& result);
OneShotResult run_one_shot(ApplicationState& state,
    MainThreadQueue& main_thread, const OneShotRequest& request);
int one_shot_exit_code(OneShotResult::Kind kind);
void submit(ApplicationState& state, std::string text,
    std::vector<Attachment> attachments = { });
void resolve_modal(ApplicationState& state, ModalResult result);
void close_modal(ApplicationState& state);
void enqueue_user_modal(ApplicationState& state, ModalPayload payload);

// MCP server persistence: validate-then-commit through the locked config
// read-modify-write, then apply to the live manager (reload, and connect
// for a newly added server). The remove/autoload forms return false when
// nothing was committed (id missing, unchanged value).
void mcp_add_server(ApplicationState& state, const McpServerConfig& server);
bool mcp_remove_server(ApplicationState& state, const std::string& id);
bool mcp_set_server_autoload(
    ApplicationState& state, const std::string& id, bool autoload);
// Adds a browser sign-in server (disabled; the flow enables it on
// success) and starts the manager's OAuth flow. False when the entry
// could not be committed.
bool mcp_begin_server_sign_in(
    ApplicationState& state, const McpServerConfig& server);
std::future<ModalResult> request_modal(
    ApplicationState& state, ModalPayload payload);
void present_front(ApplicationState& state);
void drain_queued(ApplicationState& state);
void on_turn_finished(ApplicationState& state, std::string error);
void run_slash(ApplicationState& state, std::string_view command);
// A hidden, cancellable review pass over the loaded diff: AI findings or
// the handoff report. The caller supplies the prompt and consumes the raw
// reply; everything else — model selection, the loaded-review and size
// guards, the subagent options, the transcript, the running state — is
// shared. Each pass owns its own running slot, so the two may run at the
// same time; only a repeat of the same pass is refused. `complete` runs on
// the subagent worker with the reply and returns "" when the reply had the
// expected shape, or a parse error otherwise; a non-empty result reruns the
// pass once with reasoning off before the error surfaces (unless `retry` is
// false). Returns the task id, or nullopt when the pass was rejected
// (`error` explains).
struct ReviewPassRequest {
    ReviewPass pass = ReviewPass::FINDINGS;
    std::string prompt;
    std::string label;
    std::function<std::string(std::string output)> complete;
    bool retry = true;
};

std::optional<std::size_t> run_review_pass(
    ApplicationState& state, ReviewPassRequest request, std::string& error);
// Cancels a running pass. False when nothing was running.
bool cancel_review_pass(ApplicationState& state, ReviewPass pass);
// Generate the handoff report: reconcile the plan, the user's messages, the
// build log, and the final diff into one document stored on the session.
// Reachable from any workflow phase; runs alongside an AI review pass, and
// is refused (with `error` surfaced) only while another handoff runs.
void run_handoff(ApplicationState& state);
// Write a viewer's content to <workspace-root>/.imza/<timestamp>-<name>.md
// and open it with the platform file opener. Returns the path, or an empty
// string with `error` set when the export was refused or failed.
std::string export_viewer_content(
    ApplicationState& state, const ViewerModal& payload, std::string& error);
void compact_session(ApplicationState& state, TurnSettings settings);
void interrupt(ApplicationState& state);
void delete_saved_session(
    ApplicationState& state, const std::filesystem::path& path);
// Load `path` as the active conversation: pending-work guard, save the
// current session, lock and validate the target, restore, clear runtime
// state. On failure the active conversation stays and a session error is set.
void switch_session(ApplicationState& state, const std::filesystem::path& path);
// Show the Sidechat pane; its context loads lazily on the first prompt.
void open_sidechat(ApplicationState& state);
// Hide the Sidechat pane. An in-flight turn is interrupted; the session
// persists.
void close_sidechat(ApplicationState& state);
// Load the sidechat's leading snapshot of the active conversation, once.
void ensure_sidechat_seeded(ApplicationState& state);
// Clear the sidechat's context; the next prompt reloads it. Errors when no
// sidechat exists.
void refresh_sidechat(ApplicationState& state);

} // namespace imza
