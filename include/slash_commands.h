#pragma once

#include <span>
#include <string_view>

namespace ursa {

struct SlashCommand {
    std::string_view name;
    std::string_view desc;
    enum class Action { EXIT, NEW, SYSTEM_PROMPT, CONNECT, MODEL, VARIANT,
        SUBAGENTS, SESSIONS, SKILLS };
    Action action = Action::EXIT;
};

std::span<const SlashCommand> slash_commands();
const SlashCommand* find_command(std::string_view name);

} // namespace ursa
