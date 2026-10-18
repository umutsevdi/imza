#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "common/permission_request.h"
#include "common/types.h"
#include "permissions/store.h"

namespace imza {

class Environment;
class McpSubsystem;
struct SystemEnvironment;
struct WorkspaceEnvironment;

struct PermissionContext {
    std::shared_ptr<const SystemEnvironment> system;
    std::shared_ptr<const WorkspaceEnvironment> workspace;
    PermissionStore::Snapshot grants;
    SessionMode mode = SessionMode::PLAN;
    // Loaded skills' directories: reading under one is implied by the
    // skill's approval; writing there is not.
    std::vector<std::filesystem::path> skill_directories;
    // MCP inventory for the load_mcp gate; null when the run has no MCP.
    const McpSubsystem* mcp = nullptr;
};

struct PermissionDecision {
    enum class Kind { ACCEPT, ASK, REJECT };

    Kind kind = Kind::REJECT;
    std::string reason;
};

struct FilesystemEvaluation {
    PermissionDecision decision;
    std::optional<FilesystemRequest> request;
};

PermissionContext make_permission_context(const Environment& environment,
    const PermissionStore& permissions, SessionMode mode,
    std::vector<std::filesystem::path> skill_directories = { },
    const McpSubsystem* mcp                              = nullptr);
FilesystemEvaluation evaluate_filesystem_request(
    FilesystemRequest request, const PermissionContext& context);
std::optional<ExternalGrant> filesystem_session_grant(
    const FilesystemRequest& request);
const std::filesystem::path& filesystem_target(
    const FilesystemRequest& request);
std::string_view filesystem_request_name(const FilesystemRequest& request);

} // namespace imza
