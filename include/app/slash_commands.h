#pragma once

#include <span>
#include <string>
#include <string_view>

namespace imza {

struct SlashCommand {
    enum class Action {
        EXIT,
        NEW,
        CHANGELOG,
        CONNECT,
        MODEL,
        VARIANT,
        SUBAGENTS,
        SESSIONS,
        SKILLS,
        MCP,
        COMPACT,
        MAKE_SKILL
    };

    std::string_view name;
    std::string_view desc;
    Action action = Action::EXIT;
    // Argument-taking commands complete without submitting, so the user
    // can type the argument; the rest execute on the same Enter.
    bool takes_argument = false;
};

std::span<const SlashCommand> slash_commands();
const SlashCommand* find_command(std::string_view name);

} // namespace imza
