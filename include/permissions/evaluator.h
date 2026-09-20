#pragma once

#include <chrono>
#include <filesystem>
#include <string>
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

struct ShellRequest {
    std::string command;
    std::chrono::seconds timeout { 10 };
    std::filesystem::path workspace;

    bool operator==(const ShellRequest&) const = default;
};

struct ShellEvaluation {
    PermissionDecision decision;
    ShellRequest request;
    PermissionStore::Grants session_grants;
};

PermissionEvaluation evaluate_tool_request(const ToolCallRequest& request,
    const PermissionContext& context, const Config& config,
    const std::vector<Skill>& skills, const SkillStore& loaded_skills);
ShellEvaluation evaluate_shell_request(
    ShellRequest request, const PermissionContext& context);

} // namespace imza
