#include "permissions/verdict.h"

#include <utility>

namespace imza {

SessionGrantOutcome resolve_session_grant(const ToolVerdict& verdict,
    PermissionStore::Grants grants,
    const std::function<bool(PermissionStore::Grants)>& install)
{
    if (verdict.decision != ToolDecision::ACCEPT_FOR_SESSION) {
        return { };
    }
    if (grants.empty() || !install || !install(std::move(grants))) {
        return { false, "session approval is unavailable" };
    }
    return { };
}

} // namespace imza
