#pragma once

#include <chrono>
#include <filesystem>
#include <vector>

#include "permissions/filesystem.h"
#include "permissions/store.h"

namespace imza {

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

// The shell gate: turns a command into a verdict through the read-only
// catalogs and session grants from permissions/shell_analysis.h.
ShellEvaluation evaluate_shell_request(
    ShellRequest request, const PermissionContext& context);

} // namespace imza
