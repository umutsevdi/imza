#include "agent/application_state.h"
#include "agent/flows.h"
#include "agent/slash_commands.h"
#include "core/config.h"
#include <cstdlib>
#include <filesystem>
#include <functional>

#include <doctest/doctest.h>

namespace ursa {

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
    char program[] = "ursa";
    char* argv[]   = { program };

    const CliResult result = run_cli(1, argv);

    CHECK(result.continue_as_interactive);
    CHECK(result.exit_code == 0);
    CHECK_FALSE(result.working_directory.has_value());
}

TEST_CASE("CLI returns an interactive working directory")
{
    char program[]   = "ursa";
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
    char program[]        = "ursa";
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

TEST_CASE("CLI config creates the file and opens an editor")
{
#ifdef _WIN32
    return;
#else
    const auto root
        = std::filesystem::temp_directory_path() / "ursa-cli-config-test";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    const char* previous_config = std::getenv("XDG_CONFIG_HOME");
    const char* previous_visual = std::getenv("VISUAL");
    const std::string saved_config
        = previous_config == nullptr ? "" : previous_config;
    const std::string saved_visual
        = previous_visual == nullptr ? "" : previous_visual;
    setenv("XDG_CONFIG_HOME", root.c_str(), 1);
    setenv("VISUAL", "true", 1);

    char program[]         = "ursa";
    char option[]          = "--config";
    char* argv[]           = { program, option };
    const CliResult result = run_cli(2, argv);

    CHECK_FALSE(result.continue_as_interactive);
    CHECK(result.exit_code == 0);
    CHECK(std::filesystem::is_regular_file(root / "ursa" / "config.json"));

    if (previous_config == nullptr) {
        unsetenv("XDG_CONFIG_HOME");
    } else {
        setenv("XDG_CONFIG_HOME", saved_config.c_str(), 1);
    }
    if (previous_visual == nullptr) {
        unsetenv("VISUAL");
    } else {
        setenv("VISUAL", saved_visual.c_str(), 1);
    }
    std::filesystem::remove_all(root, error);
#endif
}

TEST_CASE("CLI one-shot commands are explicit placeholders")
{
    char program[] = "ursa";
    char ask[]     = "--ask";
    char query[]   = "summarize this project";
    char* argv[]   = { program, ask, query };

    const CliResult result = run_cli(3, argv);

    CHECK_FALSE(result.continue_as_interactive);
    CHECK(result.exit_code == 2);
}

TEST_CASE("run_slash_command emits application effects")
{
    auto state = make_application_state(
        [](std::function<void()> f) { f(); }, Config { });
    bool exited = false;
    ModalPayload modal;
    std::string error;
    SlashCommandContext context { *state, [&] { exited = true; }, [] { },
        [&](ModalPayload next) { modal = std::move(next); },
        [] { return SessionsModal { }; }, [] { return SkillsModal { }; },
        [] { return std::string { }; },
        [&](std::string next) { error = std::move(next); } };

    run_slash_command(context, "/exit");
    CHECK(exited);

    run_slash_command(context, "/connect");
    REQUIRE(std::holds_alternative<ConnectModal>(modal));
    CHECK(std::get<ConnectModal>(modal).entry == ConnectModal::Entry::MANAGE);

    run_slash_command(context, "/missing");
    CHECK(error == "Unknown command: /missing.");
}

} // namespace ursa
