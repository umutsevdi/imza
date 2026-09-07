#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <json/json.h>

#include "conversation/session.h"
#include "permissions/store.h"

namespace ursa {

class Environment;
struct SystemEnvironment;
struct WorkspaceEnvironment;

struct PermissionContext {
    std::shared_ptr<const SystemEnvironment> system;
    std::shared_ptr<const WorkspaceEnvironment> workspace;
    PermissionStore::Snapshot grants;
    Session::Mode mode = Session::Mode::PLAN;
};

struct PermissionDecision {
    enum class Kind { ACCEPT, ASK, REJECT };

    Kind kind = Kind::REJECT;
    std::string reason;
};

struct FilesystemRequest {
    enum class Operation { READ, LIST, EDIT, WRITE };

    Operation operation = Operation::READ;
    std::filesystem::path target;
    Json::Value normalized_arguments;
};

struct FilesystemEvaluation {
    PermissionDecision decision;
    std::optional<FilesystemRequest> request;
};

PermissionContext permission_context(const Environment& environment,
    const PermissionStore& permissions, Session::Mode mode);
FilesystemEvaluation evaluate_filesystem_request(std::string_view tool,
    const std::string& arguments, const PermissionContext& context);
std::optional<PathGrant> filesystem_session_grant(
    const FilesystemRequest& request);

} // namespace ursa
