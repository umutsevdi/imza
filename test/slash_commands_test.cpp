#include "application_state.h"
#include "slash_commands.h"
#include "types.h"

#include <doctest/doctest.h>

namespace ursa {

TEST_CASE("slash_commands includes built-ins")
{
    const auto cmds = slash_commands();
    bool has_exit   = false;
    bool has_connect = false;
    bool has_model   = false;
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
    CHECK(find_command("/connect")->action
        == SlashCommand::Action::CONNECT);
    CHECK(find_command("/model")->action
        == SlashCommand::Action::MODEL);
    CHECK(find_command("/foo") == nullptr);
}

TEST_CASE("run_slash_command emits application effects")
{
    ApplicationState state;
    bool exited = false;
    ModalPayload modal;
    std::string error;
    SlashCommandContext context { state,
        [&] { exited = true; },
        [] { },
        [&](ModalPayload next) { modal = std::move(next); },
        [] { return SessionsModal { }; },
        [] { return SkillsModal { }; },
        [] { return std::string { }; },
        [&](std::string next) { error = std::move(next); } };

    run_slash_command(context, "/exit");
    CHECK(exited);

    run_slash_command(context, "/connect");
    REQUIRE(std::holds_alternative<ConnectModal>(modal));
    CHECK(std::get<ConnectModal>(modal).entry == ConnectModal::Entry::MANAGE);

    run_slash_command(context, "/missing");
    CHECK(error == "unknown command: /missing");
}

} // namespace ursa
