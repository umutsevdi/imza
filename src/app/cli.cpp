#include "app/flows.h"
#include "common/util.h"
#include "conversation/persistence.h"
#include "permissions/shell_analysis.h"
#include "platform/command_runner.h"
#include "platform/config.h"
#include "platform/update.h"
#include "workspace/environment.h"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <optional>
#include <print>
#include <string>
#include <vector>

namespace imza {

namespace {

    bool requests_one_shot(int argc, char** argv)
    {
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument = argv[index];
            if (argument == "--") {
                return false;
            }
            if (argument == "-a" || argument == "--ask"
                || argument.starts_with("--ask=") || argument == "-e"
                || argument == "--exec" || argument.starts_with("--exec=")) {
                return true;
            }
        }
        return false;
    }

    CliResult finished(int exit_code)
    {
        CliResult result;
        result.continue_as_interactive = false;
        result.exit_code               = exit_code;
        return result;
    }

    void print_update_banner()
    {
        if (const std::optional<std::string> version
            = cached_update(IMZA_VERSION)) {
            std::println(stderr, "<Update available v{}>", *version);
        }
    }

    int run_update_command()
    {
        std::vector<std::string> package_managers;
        detect_package_managers(package_managers);
        std::println("Checking for the latest imza release...");
        const std::optional<UpdateInfo> update
            = fetch_update(package_managers, IMZA_VERSION);
        if (!update) {
            std::println("No update available.");
            return 0;
        }
        std::println("Downloading imza {}...", update->version);
        std::error_code error;
        const std::filesystem::path temp = prepare_imza_temporary_directory(
            std::filesystem::temp_directory_path(error));
        if (error) {
            std::println(stderr, "cannot resolve temporary directory");
            return 1;
        }
        if (install_update(*update, temp) != 0) {
            std::println(
                stderr, "installation of imza {} failed", update->version);
            return 1;
        }
        std::println("Installed imza {}.", update->version);
        return 0;
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
            = run_attached_command(editor + " " + shell_quote(path));
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

        std::print("{}", format_session_list(sessions));
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

std::string format_session_list(const std::vector<SavedSession>& sessions)
{
    constexpr std::string_view id_header    = "SESSION ID";
    constexpr std::string_view saved_header = "SAVED";
    std::size_t id_width                    = id_header.size();
    std::size_t saved_width                 = saved_header.size();
    for (const SavedSession& session : sessions) {
        id_width    = std::max(id_width, session.path.stem().string().size());
        saved_width = std::max(saved_width, session.saved_at.size());
    }

    std::string output;
    const auto append_row = [&](const std::string& id, const std::string& saved,
                                std::string_view title) {
        output += id;
        output.append(id_width - id.size() + 2, ' ');
        output += saved;
        output.append(saved_width - saved.size() + 2, ' ');
        output += title;
        output += '\n';
    };
    append_row(std::string(id_header), std::string(saved_header), "TITLE");
    for (const SavedSession& session : sessions) {
        const std::string saved
            = session_file_locked(session.path) ? "locked" : session.saved_at;
        append_row(session.path.stem().string(), saved, session.title);
    }
    return output;
}

CliResult run_cli(int argc, char** argv)
{
    CLI::App app { "imza " IMZA_VERSION "\r\n"
                   "Umut Sevdi <mail@umutsevdi.com>\r\n"
                   "Open source multi-modal coding agent.",
        "imza" };
    app.set_version_flag("-v,--version", "imza " IMZA_VERSION);
    std::vector<std::string> session_arguments;
    std::string working_directory;
    std::string model;
    std::string variant;
    std::string web;
    std::string shell;
    std::string ask;
    std::string exec;
    std::vector<std::string> allowed_directories;
    std::vector<std::string> allowed_commands;
    bool config_requested = false;
    bool skip_permissions = false;
    bool update_requested = false;
    app.add_flag("-c,--config", config_requested, "Open the config file");
    app.add_flag("--skip-permissions", skip_permissions,
        "Automatically allow permission prompts for this process");
    app.add_flag("--update", update_requested,
        "Download and install the latest release");
    auto* ask_option = app.add_option("-a,--ask", ask,
                              "Run a one-shot read-only agent query")
                           ->type_name("<query>");
    auto* exec_option
        = app.add_option("-e,--exec", exec, "Run a one-shot build agent query")
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
    app.add_option("-M,--model", model, "Model to use for this session")
        ->type_name("<model>");
    app.add_option(
           "-V,--variant", variant, "Reasoning variant to use for this session")
        ->type_name("<variant>")
        ->check(CLI::IsMember({ "off", "low", "default", "high" }));
    app.add_option(
           "-W,--web", web, "Enable or disable web tools (default: true)")
        ->type_name("<bool>")
        ->check(CLI::IsMember({ "true", "false" }));
    app.add_option("-S,--shell", shell,
           "Enable or disable the shell tool (default: true)")
        ->type_name("<bool>")
        ->check(CLI::IsMember({ "true", "false" }));
    app.add_option("-D,--allow-dir", allowed_directories,
           "Allow access to one or more additional directories")
        ->type_name("<directory>...")
        ->check(CLI::ExistingDirectory);
    app.add_option("-C,--allow-cmd", allowed_commands,
           "Allow one or more shell commands for this session")
        ->type_name("<command>...");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        if (error.get_exit_code() == 0 && !requests_one_shot(argc, argv)) {
            print_update_banner();
        }
        return finished(app.exit(error));
    }

    const bool one_shot_requested
        = ask_option->count() != 0 || exec_option->count() != 0;
    const bool session_command = !session_arguments.empty()
        && (session_arguments.front() == "ls"
            || session_arguments.front() == "rm");
    if (!one_shot_requested
        && (update_requested || config_requested || session_command)) {
        print_update_banner();
    }

    std::vector<ShellCommandGrant> command_grants;
    command_grants.reserve(allowed_commands.size());
    for (const std::string& command : allowed_commands) {
        const ShellAnalysis analysis = analyze_shell(command);
        if (analysis.reuse != ShellAnalysis::Reuse::SESSION
            || analysis.invocations.size() != 1) {
            std::println(stderr, "invalid --allow-cmd value: '{}'", command);
            return finished(2);
        }
        const ShellInvocation& invocation = analysis.invocations.front();
        command_grants.push_back({ invocation.program, invocation.subcommand });
    }

    auto apply_runtime_options = [&](CliResult& result) {
        result.skip_permissions = skip_permissions;
        for (const std::string& directory : allowed_directories) {
            result.allowed_directories.push_back(
                std::filesystem::absolute(directory).lexically_normal());
        }
        result.allowed_commands = command_grants;
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
    auto apply_one_shot = [&](CliResult& result) {
        if (ask_option->count() == 0 && exec_option->count() == 0) {
            return;
        }
        result.one_shot = OneShotRequest { ask_option->count() > 0
                ? OneShotRequest::Mode::ASK
                : OneShotRequest::Mode::EXEC,
            ask_option->count() > 0 ? std::move(ask) : std::move(exec) };
    };

    if (update_requested) {
        return finished(run_update_command());
    }
    if (config_requested) {
        return finished(edit_config());
    }

    if (session_arguments.empty()) {
        CliResult result;
        apply_runtime_options(result);
        apply_one_shot(result);
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
    apply_one_shot(result);
    result.session_path = *path;
    return result;
}

RuntimeFlag runtime_flags_for(const CliResult& result)
{
    int flags = result.one_shot.has_value()
        ? RuntimeFlag::WEB | RuntimeFlag::SHELL
        : interactive_runtime_flags();
    if (!result.web.value_or(true)) {
        flags &= ~RuntimeFlag::WEB;
    }
    if (!result.shell.value_or(true)) {
        flags &= ~RuntimeFlag::SHELL;
    } else {
        flags |= RuntimeFlag::SHELL;
    }
    if (result.skip_permissions) {
        flags |= RuntimeFlag::SKIP_PERMISSIONS;
    }
    return static_cast<RuntimeFlag>(flags);
}

} // namespace imza
