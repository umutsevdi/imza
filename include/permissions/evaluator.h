#pragma once

#include <vector>

#include "common/tool_call.h"
#include "permissions/filesystem.h"

namespace ursa {

class SkillStore;
struct Config;
struct Skill;

struct PermissionEvaluation {
    PermissionDecision decision;
    ToolCallRequest request;
    PermissionStore::Grants session_grants;
};

PermissionEvaluation evaluate_tool_request(const ToolCallRequest& request,
    const PermissionContext& context, const Config& config,
    const std::vector<Skill>& skills, const SkillStore& loaded_skills);

} // namespace ursa
