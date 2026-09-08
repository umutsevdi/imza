#include "app/application_state.h"
#include "app/flows.h"
#include "app/slash_commands.h"
#include "platform/config.h"
#include <cstdlib>
#include <filesystem>
#include <functional>

#include <doctest/doctest.h>

namespace imza {

TEST_CASE("slash_commands includes built-ins")
{
    const auto cmds    = slash_commands();
    bool has_exit      = false;
    bool has_connect   = false;
    bool has_model     = false;
    bool has_subagents = false;
    for (const auto& c : cmds) {
        if (c.name == "/exit") {
            has_exit = true;
        }
        if (c.name == "/connect") {
            has_connect = true;
        }
        if (c.name == "/model") {
            has_model = true;
        }
        if (c.name == "/subagents") {
            has_subagents = true;
        }
    }
    CHECK(has_exit);
    CHECK(has_connect);
    CHECK(has_model);
    CHECK(has_subagents);
    for (const auto& c : cmds) {
        const bool known = c.action == SlashCommand::Action::EXIT
            || c.action == SlashCommand::Action::NEW
            || c.action == SlashCommand::Action::SYSTEM_PROMPT
            || c.action == SlashCommand::Action::CONNECT
            || c.action == SlashCommand::Action::MODEL
            || c.action == SlashCommand::Action::VARIANT
            || c.action == SlashCommand::Action::SUBAGENTS
            || c.action == SlashCommand::Action::SESSIONS
            || c.action == SlashCommand::Action::SKILLS;
        CHECK(known);
    }
}

TEST_CASE("slash_commands names start with slash")
{
    const auto cmds = slash_commands();
    for (const auto& c : cmds) {
        CHECK_FALSE(c.name.empty());
        CHECK(c.name.front() == '/');
    }
}

TEST_CASE("find_command matches case-insensitively")
{
    CHECK(find_command("/help") == nullptr);
    CHECK(find_command("/exit")->action == SlashCommand::Action::EXIT);
    CHECK(find_command("/new")->action == SlashCommand::Action::NEW);
    CHECK(find_command("/connect")->action == SlashCommand::Action::CONNECT);
    CHECK(find_command("/model")->action == SlashCommand::Action::MODEL);
    CHECK(find_command("/foo") == nullptr);
}

TEST_CASE("CLI continues interactively without arguments")
{
    char program[] = "imza";
    char* argv[]   = { program };

    const CliResult result = run_cli(1, argv);

    CHECK(result.continue_as_interactive);
    CHECK(result.exit_code == 0);
    CHECK_FALSE(result.working_directory.has_value());
}

TEST_CASE("CLI returns an interactive working directory")
{
    char program[]   = "imza";
    char directory[] = ".";
    char* argv[]     = { program, directory };

    const CliResult result = run_cli(2, argv);

    CHECK(result.continue_as_interactive);
    CHECK(result.exit_code == 0);
    REQUIRE(result.working_directory.has_value());
    CHECK(*result.working_directory == std::filesystem::path("."));
}

TEST_CASE("CLI returns runtime session overrides")
{
    char program[]        = "imza";
    char model_option[]   = "--model";
    char model[]          = "gpt-test";
    char variant_option[] = "--variant";
    char variant[]        = "high";
    char web_option[]     = "--web";
    char disabled[]       = "false";
    char shell_option[]   = "--shell";
    char enabled[]        = "true";
    char* argv[] = { program, model_option, model, variant_option, variant,
        web_option, disabled, shell_option, enabled };

    const CliResult result = run_cli(9, argv);

    CHECK(result.continue_as_interactive);
    CHECK(result.model == "gpt-test");
    CHECK(result.variant == "high");
    CHECK(result.web == false);
    CHECK(result.shell == true);
}

TEST_CASE("CLI exposes dangerous permission skipping in interactive mode")
{
    char program[]  = "imza";
    char skip[]     = "--skip-permissions";
    char shell[]    = "--shell";
    char disabled[] = "false";
    char* argv[]    = { program, skip, shell, disabled };

    const CliResult result  = run_cli(4, argv);
    const RuntimeFlag flags = runtime_flags_for(result);

    CHECK(result.continue_as_interactive);
    CHECK(result.skip_permissions);
    CHECK((flags & RuntimeFlag::SKIP_PERMISSIONS) != RuntimeFlag::NONE);
    CHECK((flags & RuntimeFlag::ATTENDED) != RuntimeFlag::NONE);
    CHECK((flags & RuntimeFlag::SHELL) == RuntimeFlag::NONE);
}

TEST_CASE("CLI accepts multiple allowed directories")
{
    const std::filesystem::path root
        = std::filesystem::temp_directory_path() / "imza-cli-allowed-dirs";
    const std::filesystem::path first  = root / "first";
    const std::filesystem::path second = root / "second";
    std::error_code error;
    std::filesystem::create_directories(first, error);
    REQUIRE_FALSE(error);
    std::filesystem::create_directories(second, error);
    REQUIRE_FALSE(error);

    std::string first_string  = first.string();
    std::string second_string = second.string();
    char program[]            = "imza";
    char option[]             = "--allow-dir";
    char* argv[]
        = { program, option, first_string.data(), second_string.data() };

    const CliResult result = run_cli(4, argv);

    REQUIRE(result.continue_as_interactive);
    REQUIRE(result.allowed_directories.size() == 2);
    CHECK(result.allowed_directories[0]
        == std::filesystem::weakly_canonical(first));
    CHECK(result.allowed_directories[1]
        == std::filesystem::weakly_canonical(second));

    std::filesystem::remove_all(root, error);
}

TEST_CASE("CLI config creates the file and opens an editor")
{
#ifdef _WIN32
    return;
#else
    const auto root
        = std::filesystem::temp_directory_path() / "imza-cli-config-test";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    const char* previous_config = std::getenv("XDG_DATA_HOME");
    const char* previous_visual = std::getenv("VISUAL");
    const std::string saved_config
        = previous_config == nullptr ? "" : previous_config;
    const std::string saved_visual
        = previous_visual == nullptr ? "" : previous_visual;
    setenv("XDG_DATA_HOME", root.c_str(), 1);
    setenv("VISUAL", "true", 1);

    char program[]         = "imza";
    char option[]          = "--config";
    char* argv[]           = { program, option };
    const CliResult result = run_cli(2, argv);

    CHECK_FALSE(result.continue_as_interactive);
    CHECK(result.exit_code == 0);
    CHECK(std::filesystem::is_regular_file(root / "imza" / "config.json"));

    if (previous_config == nullptr) {
        unsetenv("XDG_DATA_HOME");
    } else {
        setenv("XDG_DATA_HOME", saved_config.c_str(), 1);
    }
    if (previous_visual == nullptr) {
        unsetenv("VISUAL");
    } else {
        setenv("VISUAL", saved_visual.c_str(), 1);
    }
    std::filesystem::remove_all(root, error);
#endif
}

TEST_CASE("CLI parses ask as an unattended Plan one-shot")
{
    char program[] = "imza";
    char ask[]     = "--ask";
    char query[]   = "summarize this project";
    char* argv[]   = { program, ask, query };

    const CliResult result  = run_cli(3, argv);
    const RuntimeFlag flags = runtime_flags_for(result);

    REQUIRE(result.one_shot.has_value());
    CHECK(result.one_shot->mode == OneShotRequest::Mode::ASK);
    CHECK(result.one_shot->query == query);
    CHECK((flags & RuntimeFlag::WEB) != RuntimeFlag::NONE);
    CHECK((flags & RuntimeFlag::SHELL) != RuntimeFlag::NONE);
    CHECK((flags & RuntimeFlag::ATTENDED) == RuntimeFlag::NONE);
}

TEST_CASE("CLI accepts command and command-subcommand startup grants")
{
    char program[]       = "imza";
    char option[]        = "--allow-cmd";
    char command[]       = "git status";
    char whole_command[] = "cmake";
    char* argv[]         = { program, option, command, whole_command };

    const CliResult result = run_cli(4, argv);

    REQUIRE(result.allowed_commands.size() == 2);
    CHECK(
        result.allowed_commands[0] == (ShellCommandGrant { "git", "status" }));
    CHECK(result.allowed_commands[1]
        == (ShellCommandGrant { "cmake", std::nullopt }));
}

TEST_CASE("CLI rejects compound command startup grants")
{
    char program[] = "imza";
    char option[]  = "--allow-cmd";
    char command[] = "git status && make";
    char* argv[]   = { program, option, command };

    const CliResult result = run_cli(3, argv);

    CHECK_FALSE(result.continue_as_interactive);
    CHECK(result.exit_code == 2);
}

TEST_CASE("CLI parses exec with explicit shell access")
{
    char program[] = "imza";
    char exec[]    = "--exec";
    char query[]   = "build this project";
    char shell[]   = "--shell";
    char enabled[] = "true";
    char* argv[]   = { program, exec, query, shell, enabled };

    const CliResult result  = run_cli(5, argv);
    const RuntimeFlag flags = runtime_flags_for(result);

    REQUIRE(result.one_shot.has_value());
    CHECK(result.one_shot->mode == OneShotRequest::Mode::EXEC);
    CHECK(result.one_shot->query == query);
    CHECK((flags & RuntimeFlag::SHELL) != RuntimeFlag::NONE);
    CHECK((flags & RuntimeFlag::ATTENDED) == RuntimeFlag::NONE);
}

TEST_CASE("run_slash emits application effects")
{
    auto state = make_application_state(
        [](std::function<void()> f) { f(); }, Config { });
    bool exited    = false;
    state->on_exit = [&] { exited = true; };

    run_slash(*state, "/exit");
    CHECK(exited);

    run_slash(*state, "/connect");
    const ModalPayload modal = state->session->modal();
    REQUIRE(std::holds_alternative<ConnectModal>(modal));
    CHECK(std::get<ConnectModal>(modal).entry == ConnectModal::Entry::MANAGE);

    run_slash(*state, "/missing");
    CHECK(state->session->error() == "Unknown command: /missing.");
}

} // namespace imza
