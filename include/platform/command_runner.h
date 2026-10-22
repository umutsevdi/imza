#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>

namespace imza {

struct CommandResult {
    std::string output;
    int exit_code  = 0;
    bool timed_out = false;
    bool spawned   = false;
};

inline bool command_ok(const CommandResult& result)
{
    return result.spawned && !result.timed_out && result.exit_code == 0;
}

// MERGE keeps run_command's usual behavior of folding stderr into `output`
// (a shell command's diagnostics are part of what the user asked to see).
// STDOUT_ONLY discards stderr: for commands whose stdout is parsed - git
// emits CRLF-conversion warnings on stderr that would corrupt the parse -
// and a discarded stream can never fill a pipe and block the child.
enum class StderrCapture { MERGE, STDOUT_ONLY };

std::string shell_quote(const std::filesystem::path& path);
CommandResult run_command(const std::string& command,
    std::chrono::seconds timeout,
    const std::filesystem::path& working_directory = { },
    std::string_view stdin_data                    = { },
    StderrCapture stderr_capture                   = StderrCapture::MERGE);
CommandResult run_attached_command(const std::string& command);
// Launches `command` detached: returns as soon as the child is spawned, so
// the caller keeps its terminal. Used to hand a file to the user's editor
// from inside the TUI.
bool spawn_detached_command(const std::string& command);

// Launches `command` detached in its own process group with stdio on
// /dev/null: output must be redirected by the command itself. Blocks until
// the process exits, `timeout` elapses, or `stop` is requested (then the
// whole group is killed); returns false only when the child could not be
// spawned. `on_spawned` fires right after a successful fork, so a caller
// can report success without waiting for the process to finish.
bool spawn_background_command(const std::string& command,
    std::chrono::seconds timeout,
    const std::filesystem::path& working_directory, std::stop_token stop = { },
    const std::function<void()>& on_spawned = { });
bool open_browser(std::string_view url);

} // namespace imza
