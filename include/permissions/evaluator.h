#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/tool_call.h"
#include "permissions/filesystem.h"

namespace imza {

class SkillStore;
struct Config;
struct Skill;

// The model-facing roster tools the permission layer knows. Adding a
// roster tool means adding it here: classify_roster_tool returns nullopt
// for anything else, and evaluate_tool_request rejects it as unpolicied.
enum class RosterTool { SKILL, SUBAGENT, LUA };

std::optional<RosterTool> classify_roster_tool(std::string_view name);

struct PermissionEvaluation {
    PermissionDecision decision;
    ToolCallRequest request;
    PermissionStore::Grants session_grants;
};

PermissionEvaluation evaluate_tool_request(const ToolCallRequest& request,
    const PermissionContext& context, const Config& config,
    const std::vector<Skill>& skills, const SkillStore& loaded_skills);

} // namespace imza
