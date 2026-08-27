#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace ursa {

struct SlashCommand {
    std::string name;
    std::string desc;
    enum class Action { EXIT, HELP, SYSTEM_PROMPT, CONNECT, MODEL, VARIANT };
    Action action = Action::HELP;
};

std::vector<SlashCommand> slash_commands();
const SlashCommand* find_command(
    const std::vector<SlashCommand>& commands, std::string_view name);

} // namespace ursa