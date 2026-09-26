#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/application_state.h"
#include "permissions/store.h"

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
    bool skip_permissions = false;
};

CliResult run_cli(int argc, char** argv);
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
std::future<ModalResult> request_modal(
    ApplicationState& state, ModalPayload payload);
void present_front(ApplicationState& state);
void drain_queued(ApplicationState& state);
void on_turn_finished(ApplicationState& state, std::string error);
void run_slash(ApplicationState& state, std::string_view command);
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
