#pragma once

#include <span>
#include <string_view>

namespace ursa {

struct SlashCommand {
    std::string_view name;
    std::string_view desc;
    enum class Action { EXIT, HELP, SYSTEM_PROMPT, CONNECT, MODEL, VARIANT, SESSIONS };
    Action action = Action::HELP;
};

std::span<const SlashCommand> slash_commands();
const SlashCommand* find_command(std::string_view name);

} // namespace ursa
