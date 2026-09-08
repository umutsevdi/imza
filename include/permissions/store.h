#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "common/types.h"
#include "common/imza_signal.h"

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
    bool matches(const ShellCommandGrant& grant) const;
    bool matches(const SkillGrant& grant) const;
    void clear();
    [[nodiscard]] Signal<>::Subscription subscribe_to_grants_change(
        Signal<>::Callback callback);

private:
    static bool _covers(
        const PermissionGrant& stored, const PermissionGrant& requested);
    mutable std::mutex _mutex;
    Snapshot _grants;
    Signal<> _changed;
};

} // namespace imza
