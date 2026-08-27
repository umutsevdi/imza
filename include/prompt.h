#pragma once

#include <string>
#include <string_view>

#include "environment.h"

namespace ursa {

constexpr std::string_view PLAN_REMINDER_TAG
    = "<system-reminder id=\"plan-mode\">";
constexpr std::string_view BUILD_REMINDER_TAG
    = "<system-reminder id=\"build-mode\">";

std::string build_system_prompt(const SystemEnvironment* sys,
    const WorkspaceEnvironment* ws);
std::string_view plan_mode_reminder();
std::string_view build_mode_reminder();

} // namespace ursa
