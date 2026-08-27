#include "commands.h"
#include "types.h"

#include <doctest/doctest.h>

namespace ursa {

TEST_CASE("slash_commands includes built-ins")
{
    const auto cmds = slash_commands();
    bool has_help   = false;
    bool has_exit   = false;
    bool has_connect = false;
    bool has_model   = false;
    for (const auto& c : cmds) {
        if (c.name == "/help") {
            has_help = true;
        }
        if (c.name == "/exit") {
            has_exit = true;
        }
        if (c.name == "/connect") {
            has_connect = true;
        }
        if (c.name == "/model") {
            has_model = true;
        }
    }
    CHECK(has_help);
    CHECK(has_exit);
    CHECK(has_connect);
    CHECK(has_model);
    for (const auto& c : cmds) {
        const bool known = c.action == SlashCommand::Action::EXIT
            || c.action == SlashCommand::Action::HELP
            || c.action == SlashCommand::Action::SYSTEM_PROMPT
            || c.action == SlashCommand::Action::CONNECT
            || c.action == SlashCommand::Action::MODEL
            || c.action == SlashCommand::Action::VARIANT;
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
    const auto cmds = slash_commands();
    CHECK(find_command(cmds, "/help") != nullptr);
    CHECK(find_command(cmds, "/HELP") != nullptr);
    CHECK(find_command(cmds, "/exit")->action == SlashCommand::Action::EXIT);
    CHECK(find_command(cmds, "/connect")->action
        == SlashCommand::Action::CONNECT);
    CHECK(find_command(cmds, "/model")->action
        == SlashCommand::Action::MODEL);
    CHECK(find_command(cmds, "/foo") == nullptr);
}

} // namespace ursa
