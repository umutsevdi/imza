#include "app/flows.h"
#include "common/util.h"
#include "conversation/persistence.h"
#include "platform/command_runner.h"
#include "platform/config.h"

#include <CLI/CLI.hpp>

#include <optional>
#include <print>
#include <string>
#include <vector>

namespace ursa {

namespace {

    CliResult finished(int exit_code)
    {
        CliResult result;
        result.continue_as_interactive = false;
        result.exit_code               = exit_code;
        return result;
    }

    std::string quoted_path(const std::filesystem::path& path)
    {
#ifdef _WIN32
        return "\"" + path.string() + "\"";
#else
        std::string quoted = "'";
        for (const char character : path.string()) {
            if (character == '\'') {
                quoted += "'\\''";
            } else {
                quoted += character;
            }
        }
        return quoted + "'";
#endif
    }

    int edit_config()
    {
        const std::filesystem::path path = config_path();
        std::error_code error;
        const bool exists = std::filesystem::exists(path, error);
        if (error || (!exists && save_config(path, Config { }) != Status::OK)) {
            std::println(
                stderr, "failed to create config file '{}'", path.string());
            return 1;
        }

        std::string editor = env_or_empty("VISUAL");
        if (editor.empty()) {
            editor = env_or_empty("EDITOR");
        }
        if (editor.empty()) {
#if defined(_WIN32)
            editor = "notepad.exe";
#elif defined(__APPLE__)
            editor = "open -t";
#else
            editor = "xdg-open";
#endif
        }
        const CommandResult result
            = run_attached_command(editor + " " + quoted_path(path));
        if (!result.spawned) {
            std::println(stderr, "failed to start config editor");
            return 1;
        }
        if (result.exit_code != 0) {
            std::println(stderr, "config editor exited with status {}",
                result.exit_code);
        }
        return result.exit_code;
    }

    int list_sessions()
    {
        const std::vector<SavedSession> sessions = saved_sessions();
        if (sessions.empty()) {
            std::println("No saved sessions.");
            return 0;
        }

        std::println("SESSION ID\tSAVED\tTITLE");
        for (const auto& session : sessions) {
            std::println("{}\t{}\t{}", session.path.stem().string(),
                session.saved_at, session.title);
        }
        return 0;
    }

    std::optional<std::filesystem::path> find_session(
        const std::vector<SavedSession>& sessions, const std::string& id)
    {
        for (const auto& session : sessions) {
            if (session.path.stem() == id) {
                return session.path;
            }
        }
        return std::nullopt;
    }

    int remove_session(const std::filesystem::path& path, const std::string& id)
    {
        switch (delete_saved_session(path)) {
        case DeleteSessionResult::OK:
            std::println("Deleted session {}.", id);
            return 0;
        case DeleteSessionResult::INVALID_PATH:
            std::println(stderr, "invalid session path for '{}'", id);
            return 2;
        case DeleteSessionResult::REMOVE_FAILED:
            std::println(stderr, "failed to delete session '{}'", id);
            return 2;
        }
        return 2;
    }

} // namespace

CliResult run_cli(int argc, char** argv)
{
    CLI::App app { "ursa " URSA_VERSION "\r\n"
                   "Umut Sevdi <mail@umutsevdi.com>\r\n"
                   "Open source multi-modal coding agent.",
        "ursa" };
    app.set_version_flag("-v,--version", "ursa " URSA_VERSION);
    std::vector<std::string> session_arguments;
    std::string working_directory;
    std::string model;
    std::string variant;
    std::string web;
    std::string shell;
    std::string ask;
    std::string exec;
    bool config_requested = false;
    app.add_flag("-c,--config", config_requested, "Open the config file");
    auto* ask_option  = app.add_option("--ask", ask,
                               "Run a one-shot read-only agent query "
                               "(unimplemented)")
                            ->type_name("<query>");
    auto* exec_option = app.add_option("--exec", exec,
                               "Run a one-shot build agent query "
                               "(unimplemented)")
                            ->type_name("<query>");
    ask_option->excludes(exec_option);
    app.add_option("-s,--session", session_arguments,
           "Open a session by ID, list with 'ls', or delete with 'rm ID'")
        ->type_name("<id>|ls|rm <id>")
        ->expected(1, 2);
    app.add_option("directory", working_directory,
           "Working directory for the interactive or command session")
        ->type_name("")
        ->check(CLI::ExistingDirectory);
    app.add_option("--model", model, "Model to use for this session")
        ->type_name("<model>");
    app.add_option(
           "--variant", variant, "Reasoning variant to use for this session")
        ->type_name("<variant>")
        ->check(CLI::IsMember({ "off", "low", "default", "high" }));
    app.add_option("--web", web, "Enable or disable web tools (default: true)")
        ->type_name("<bool>")
        ->check(CLI::IsMember({ "true", "false" }));
    app.add_option("--shell", shell,
           "Enable or disable the shell tool (interactive default: true)")
        ->type_name("<bool>")
        ->check(CLI::IsMember({ "true", "false" }));

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        return finished(app.exit(error));
    }

    auto apply_runtime_options = [&](CliResult& result) {
        if (!working_directory.empty()) {
            result.working_directory = working_directory;
        }
        if (!model.empty()) {
            result.model = model;
        }
        if (!variant.empty()) {
            result.variant = variant;
        }
        if (!web.empty()) {
            result.web = web == "true";
        }
        if (!shell.empty()) {
            result.shell = shell == "true";
        }
    };

    if (config_requested) {
        return finished(edit_config());
    }
    if (ask_option->count() > 0 || exec_option->count() > 0) {
        std::println("unimplemented");
        return finished(2);
    }

    if (session_arguments.empty()) {
        CliResult result;
        apply_runtime_options(result);
        return result;
    }
    if (session_arguments.size() == 1 && session_arguments.front() == "ls") {
        return finished(list_sessions());
    }
    if (session_arguments.front() == "rm" && session_arguments.size() != 2) {
        std::println(stderr, "--session rm requires a session ID");
        return finished(2);
    }
    if (session_arguments.size() != 1 && session_arguments.front() != "rm") {
        std::println(stderr, "invalid --session arguments");
        return finished(2);
    }

    const std::string& id = session_arguments.back();
    const auto path       = find_session(saved_sessions(), id);
    if (!path) {
        std::println(stderr, "session not found: {}", id);
        return finished(2);
    }
    if (session_arguments.front() == "rm") {
        return finished(remove_session(*path, id));
    }

    CliResult result;
    apply_runtime_options(result);
    result.session_path = *path;
    return result;
}

} // namespace ursa
