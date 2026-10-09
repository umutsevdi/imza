#pragma once

#include <functional>
#include <string>

#include "common/tool_call.h"
#include "permissions/store.h"

namespace imza {

// How a session-scoped approval resolves. Both the Lua binding gate and the
// turn runner install the same kind of grant and must agree on the failure
// wording, so the shared policy lives here.
struct SessionGrantOutcome {
    bool ok = true;
    std::string reason;
};

// Installs `grants` when the verdict is ACCEPT_FOR_SESSION. `install` performs
// the store write; the caller supplies it so this stays free of store
// ownership.
SessionGrantOutcome resolve_session_grant(const ToolVerdict& verdict,
    PermissionStore::Grants grants,
    const std::function<bool(PermissionStore::Grants)>& install);

} // namespace imza
