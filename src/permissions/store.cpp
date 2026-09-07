#include "permissions/store.h"

#include <algorithm>
#include <type_traits>

namespace ursa {

namespace {

    bool is_descendant(const std::filesystem::path& path,
        const std::filesystem::path& directory)
    {
        const std::filesystem::path relative
            = path.lexically_relative(directory);
        return path == directory
            || (!relative.empty() && *relative.begin() != "..");
    }

    bool normalize_grant(PermissionGrant& grant)
    {
        return std::visit(
            [](auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, ShellCommandGrant>) {
                    if (value.program.empty() || value.working_root.empty()
                        || !value.working_root.is_absolute()) {
                        return false;
                    }
                    std::error_code error;
                    value.working_root = std::filesystem::weakly_canonical(
                        value.working_root, error);
                    return !error;
                } else {
                    if (!value.path.is_absolute() || value.path.empty()) {
                        return false;
                    }
                    std::error_code error;
                    value.path
                        = std::filesystem::weakly_canonical(value.path, error);
                    return !error;
                }
            },
            grant);
    }

} // namespace

PermissionStore::PermissionStore()
    : _grants(std::make_shared<const Grants>())
{
}

PermissionStore::Snapshot PermissionStore::snapshot() const
{
    std::lock_guard lock(_mutex);
    return _grants;
}

bool PermissionStore::install(Grants grants)
{
    if (!std::all_of(grants.begin(), grants.end(), normalize_grant)) {
        return false;
    }
    if (grants.empty()) {
        return true;
    }
    std::lock_guard lock(_mutex);
    if (std::all_of(grants.begin(), grants.end(), [&](const auto& requested) {
            return std::any_of(_grants->begin(), _grants->end(),
                [&](const auto& stored) { return _covers(stored, requested); });
        })) {
        return true;
    }
    Grants next = *_grants;
    for (PermissionGrant& grant : grants) {
        if (std::any_of(next.begin(), next.end(),
                [&](const auto& stored) { return _covers(stored, grant); })) {
            continue;
        }
        std::erase_if(
            next, [&](const auto& stored) { return _covers(grant, stored); });
        next.push_back(std::move(grant));
    }
    _grants = std::make_shared<const Grants>(std::move(next));
    return true;
}

bool PermissionStore::matches(const PathGrant& grant) const
{
    const Snapshot grants = snapshot();
    return std::any_of(grants->begin(), grants->end(), [&](const auto& entry) {
        const auto* stored = std::get_if<PathGrant>(&entry);
        return stored != nullptr && _matches(*stored, grant);
    });
}

bool PermissionStore::matches(const ShellCommandGrant& grant) const
{
    const Snapshot grants = snapshot();
    return std::any_of(grants->begin(), grants->end(), [&](const auto& entry) {
        const auto* stored = std::get_if<ShellCommandGrant>(&entry);
        return stored != nullptr && *stored == grant;
    });
}

bool PermissionStore::matches(const SkillGrant& grant) const
{
    const Snapshot grants = snapshot();
    return std::any_of(grants->begin(), grants->end(), [&](const auto& entry) {
        const auto* stored = std::get_if<SkillGrant>(&entry);
        return stored != nullptr && *stored == grant;
    });
}

void PermissionStore::clear()
{
    std::lock_guard lock(_mutex);
    if (!_grants->empty()) {
        _grants = std::make_shared<const Grants>();
    }
}

std::size_t PermissionStore::size() const { return snapshot()->size(); }

bool PermissionStore::_covers(
    const PermissionGrant& stored, const PermissionGrant& requested)
{
    if (stored.index() != requested.index()) {
        return false;
    }
    if (const auto* path = std::get_if<PathGrant>(&stored)) {
        const auto& other = std::get<PathGrant>(requested);
        if (path->access != other.access) {
            return false;
        }
        return path->target == PathGrant::Target::DIRECTORY
            ? is_descendant(other.path, path->path)
            : other.target == PathGrant::Target::FILE
                && path->path == other.path;
    }
    if (const auto* command = std::get_if<ShellCommandGrant>(&stored)) {
        const auto& other = std::get<ShellCommandGrant>(requested);
        return *command == other;
    }
    return std::get<SkillGrant>(stored) == std::get<SkillGrant>(requested);
}

bool PermissionStore::_matches(
    const PathGrant& stored, const PathGrant& requested)
{
    if (stored.access != requested.access) {
        return false;
    }
    return stored.target == PathGrant::Target::DIRECTORY
        ? is_descendant(requested.path, stored.path)
        : stored.path == requested.path;
}

} // namespace ursa
