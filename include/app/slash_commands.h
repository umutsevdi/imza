#pragma once

#include <span>
#include <string>
#include <string_view>

namespace ursa {

struct SlashCommand {
    enum class Action {
        EXIT,
        NEW,
        SYSTEM_PROMPT,
        CONNECT,
        MODEL,
        VARIANT,
        SUBAGENTS,
        SESSIONS,
        SKILLS
    };

    std::string_view name;
    std::string_view desc;
    Action action = Action::EXIT;
};

std::span<const SlashCommand> slash_commands();
const SlashCommand* find_command(std::string_view name);

} // namespace ursa
