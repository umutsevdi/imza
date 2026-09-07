#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

#include "common/types.h"

namespace ursa {

struct PathGrant {
    enum class Access { READ, WRITE };
    enum class Target { FILE, DIRECTORY };

    Access access = Access::READ;
    Target target = Target::FILE;
    std::filesystem::path path;

    bool operator==(const PathGrant&) const = default;
};

struct ShellCommandGrant {
    std::string program;
    std::vector<std::string> argv;
    std::filesystem::path working_root;

    bool operator==(const ShellCommandGrant&) const = default;
};

struct SkillGrant {
    std::filesystem::path path;

    bool operator==(const SkillGrant&) const = default;
};

using PermissionGrant = std::variant<PathGrant, ShellCommandGrant, SkillGrant>;

class PermissionStore final : public ApplicationComponent {
public:
    using Grants   = std::vector<PermissionGrant>;
    using Snapshot = std::shared_ptr<const Grants>;

    PermissionStore();

    Snapshot snapshot() const;
    bool install(Grants grants);
    bool matches(const PathGrant& grant) const;
    bool matches(const ShellCommandGrant& grant) const;
    bool matches(const SkillGrant& grant) const;
    void clear();
    std::size_t size() const;

private:
    static bool _covers(
        const PermissionGrant& stored, const PermissionGrant& requested);
    static bool _matches(const PathGrant& stored, const PathGrant& requested);
    mutable std::mutex _mutex;
    Snapshot _grants;
};

} // namespace ursa
