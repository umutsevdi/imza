#pragma once

#include <string>
#include <string_view>

#include "core/config.h"
#include "subsystems/environment.h"

namespace ursa {

struct ApplicationState;

std::string build_system_prompt(const SystemEnvironment* sys,
    const WorkspaceEnvironment* ws, const Config* config = nullptr);
std::string build_subagent_system_prompt(const SystemEnvironment* sys,
    const WorkspaceEnvironment* ws, SubagentRole role,
    const Config* config = nullptr);
std::string full_system_prompt(const ApplicationState& state);
std::string title_prompt(std::string_view request);

} // namespace ursa
