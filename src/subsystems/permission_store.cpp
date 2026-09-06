#include "subsystems/permission_store.h"

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
    if (!std::all_of(grants.begin(), grants.end(), _valid)) {
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
        return stored != nullptr && _matches(*stored, grant);
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
        if (command->program != other.program
            || command->working_root != other.working_root) {
            return false;
        }
        if (command->match == ShellCommandGrant::Match::EXACT) {
            return other.match == ShellCommandGrant::Match::EXACT
                && command->argv == other.argv;
        }
        return other.argv.size() >= command->argv.size()
            && std::equal(
                command->argv.begin(), command->argv.end(), other.argv.begin());
    }
    return std::get<SkillGrant>(stored) == std::get<SkillGrant>(requested);
}

bool PermissionStore::_valid(const PermissionGrant& grant)
{
    return std::visit(
        [](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, PathGrant>) {
                return value.path.is_absolute() && !value.path.empty()
                    && value.path.lexically_normal() == value.path;
            } else if constexpr (std::is_same_v<T, ShellCommandGrant>) {
                return !value.program.empty()
                    && (!value.working_root
                        || value.working_root->is_absolute());
            } else {
                return value.path.is_absolute() && !value.path.empty();
            }
        },
        grant);
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

bool PermissionStore::_matches(
    const ShellCommandGrant& stored, const ShellCommandGrant& requested)
{
    if (stored.program != requested.program
        || stored.working_root != requested.working_root) {
        return false;
    }
    if (stored.match == ShellCommandGrant::Match::EXACT) {
        return stored.argv == requested.argv;
    }
    return requested.argv.size() >= stored.argv.size()
        && std::equal(
            stored.argv.begin(), stored.argv.end(), requested.argv.begin());
}

} // namespace ursa
