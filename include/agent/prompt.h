#pragma once

#include <string>
#include <string_view>

#include "core/config.h"
#include "environment/environment.h"

namespace ursa {

struct ApplicationState;

constexpr std::string_view PLAN_REMINDER_TAG
    = "<system-reminder id=\"plan-mode\">";
constexpr std::string_view BUILD_REMINDER_TAG
    = "<system-reminder id=\"build-mode\">";

std::string build_system_prompt(const SystemEnvironment* sys,
    const WorkspaceEnvironment* ws, const Config* config = nullptr);
std::string build_subagent_system_prompt(const SystemEnvironment* sys,
    const WorkspaceEnvironment* ws, SubagentRole role,
    const Config* config = nullptr);
std::string full_system_prompt(const ApplicationState& state);
std::string_view plan_mode_reminder();
std::string_view build_mode_reminder();
std::string title_prompt(std::string_view request);

} // namespace ursa
