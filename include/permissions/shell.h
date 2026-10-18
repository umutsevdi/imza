#pragma once

#include <chrono>
#include <filesystem>
#include <vector>

#include "common/permission_request.h"
#include "permissions/filesystem.h"
#include "permissions/permission_store.h"

namespace imza {

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
