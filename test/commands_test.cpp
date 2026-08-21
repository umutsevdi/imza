#include "commands.h"
#include "types.h"

#include <doctest/doctest.h>

namespace ursa {

TEST_CASE("slash_commands includes built-ins")
{
    Config cfg;
    const auto cmds = slash_commands(cfg);
    bool has_help   = false;
    bool has_exit   = false;
    for (const auto& c : cmds) {
        if (c.name == "/help") {
            has_help = true;
        }
        if (c.name == "/exit") {
            has_exit = true;
        }
    }
    CHECK(has_help);
    CHECK(has_exit);
    for (const auto& c : cmds) {
        CHECK(c.action != SlashCommand::Action::SKILL);
    }
}

TEST_CASE("slash_commands names start with slash")
{
    Config cfg;
    const auto cmds = slash_commands(cfg);
    for (const auto& c : cmds) {
        CHECK_FALSE(c.name.empty());
        CHECK(c.name.front() == '/');
    }
}

TEST_CASE("find_command matches case-insensitively")
{
    Config cfg;
    const auto cmds = slash_commands(cfg);
    CHECK(find_command(cmds, "/help") != nullptr);
    CHECK(find_command(cmds, "/HELP") != nullptr);
    CHECK(find_command(cmds, "/exit")->action == SlashCommand::Action::EXIT);
    CHECK(find_command(cmds, "/foo") == nullptr);
}

} // namespace ursa
