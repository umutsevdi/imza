#pragma once

#include <string>
#include <string_view>

#include "platform/config.h"
#include "turn/prompts.h"
#include "workspace/environment.h"

namespace imza {

struct ApplicationState;

std::string build_system_prompt(const PromptStore& prompts,
    const SystemEnvironment* sys, const WorkspaceEnvironment* ws,
    const Config* config = nullptr);
std::string build_subagent_system_prompt(const PromptStore& prompts,
    const SystemEnvironment* sys, const WorkspaceEnvironment* ws,
    SubagentRole role, const Config* config = nullptr);
std::string title_prompt(const PromptStore& prompts, std::string_view request);
std::string plan_mode_reminder(const PromptStore& prompts);
std::string build_mode_reminder(const PromptStore& prompts);
std::string full_system_prompt(const ApplicationState& state);

} // namespace imza
