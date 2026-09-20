#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "common/imza_signal.h"
#include "common/types.h"

namespace imza {

using ExternalGrant = std::filesystem::path;

struct ShellCommandGrant {
    std::string program;
    std::optional<std::string> subcommand;

    bool operator==(const ShellCommandGrant&) const = default;
};

struct SkillGrant {
    std::filesystem::path path;

    bool operator==(const SkillGrant&) const = default;
};

using PermissionGrant
    = std::variant<ExternalGrant, ShellCommandGrant, SkillGrant>;

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

private:
    mutable std::mutex _mutex;
    Snapshot _grants;
    Signal<> _changed;
};

// True when `stored` already authorizes everything `requested` covers:
// a directory grant contains the requested directory, a program-level
// shell grant covers any of its subcommands, skill grants match exactly.
bool grant_covers(
    const PermissionGrant& stored, const PermissionGrant& requested);
bool grants_cover(
    const PermissionStore::Grants& grants, const PermissionGrant& requested);
inline bool grants_cover(
    const PermissionStore::Snapshot& grants, const PermissionGrant& requested)
{
    return grants && grants_cover(*grants, requested);
}

} // namespace imza
