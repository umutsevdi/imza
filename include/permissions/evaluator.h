#pragma once

#include <vector>

#include "common/tool_call.h"
#include "permissions/filesystem.h"

namespace imza {

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

// Shell-only slice of the evaluator: analyze, validate, classify grants.
// Called by the lua tool.shell binding directly.
PermissionEvaluation evaluate_shell_request(
    const ToolCallRequest& request, const PermissionContext& context);

} // namespace imza
