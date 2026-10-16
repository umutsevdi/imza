#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "common/imza_signal.h"
#include "common/permission_request.h"
#include "common/types.h"
#include "platform/config.h"

namespace imza {

using ExternalGrant = std::filesystem::path;

struct SkillGrant {
    std::filesystem::path path;

    bool operator==(const SkillGrant&) const = default;
};

// Covers load_mcp and the connect it performs for one server id.
struct McpGrant {
    std::string server_id;

    bool operator==(const McpGrant&) const = default;
};

using PermissionGrant
    = std::variant<ExternalGrant, ShellCommandGrant, SkillGrant, McpGrant>;

class PermissionStore final : public ApplicationComponent {
public:
    using Grants   = std::vector<PermissionGrant>;
    using Snapshot = std::shared_ptr<const Grants>;

    PermissionStore();

    Snapshot snapshot() const;
    bool install(Grants grants);
    void clear();
    [[nodiscard]] Signal<>::Subscription subscribe_to_grants_change(
        Signal<>::Callback callback);

    // Live approval-skipping switch shared by every session on this
    // store (main, children, sidechat). Set once from
    // --skip-permissions at startup, flipped at runtime by /auto.
    bool skip_permissions() const;
    void set_skip_permissions(bool skip);
    [[nodiscard]] Signal<>::Subscription subscribe_to_skip_change(
        Signal<>::Callback callback);

private:
    mutable std::mutex _mutex;
    Snapshot _grants;
    std::atomic<bool> _skip_permissions { false };
    Signal<> _changed;
    Signal<> _skip_changed;
};

// True when `stored` already authorizes everything `requested` covers:
// a directory grant contains the requested directory, a program-level
// shell grant covers any of its subcommands, skill and MCP grants match
// exactly.
bool grant_covers(
    const PermissionGrant& stored, const PermissionGrant& requested);
bool grants_cover(
    const PermissionStore::Grants& grants, const PermissionGrant& requested);
inline bool grants_cover(
    const PermissionStore::Snapshot& grants, const PermissionGrant& requested)
{
    return grants && grants_cover(*grants, requested);
}

// Grants declared by `config.allow`, ready for PermissionStore::install.
// Directories that no longer exist are dropped rather than failing the
// batch: a stale allowlist entry must not block startup.
PermissionStore::Grants allow_grants(const AllowConfig& allow);

} // namespace imza
