#pragma once

#include <string>
#include <vector>

#include "types.h"

namespace ursa {

struct SlashCommand {
    std::string name;
    std::string desc;
    enum class Action { EXIT, HELP, SETTINGS, SKILL };
    Action action = Action::SKILL;
};

std::vector<SlashCommand> slash_commands(const Config& cfg);

const SlashCommand* find_command(
    const std::vector<SlashCommand>& commands, std::string_view name);

} // namespace ursa
