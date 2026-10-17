#include "permissions/store.h"

#include <algorithm>
#include <system_error>
#include <type_traits>

#include "common/util.h"
#include "permissions/shell_analysis.h"

namespace imza {

namespace {

    bool normalize_grant(PermissionGrant& grant)
    {
        return std::visit(
            [](auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, ExternalGrant>) {
                    if (value.empty() || !value.is_absolute()) {
                        return false;
                    }
                    std::error_code error;
                    value = std::filesystem::weakly_canonical(value, error);
                    return !error && std::filesystem::is_directory(value, error)
                        && !error;
                } else if constexpr (std::is_same_v<T, ShellCommandGrant>) {
                    // A launcher grant would authorize every command the
                    // program can run, so it is refused at the store: the
                    // analyzer unwraps launchers into classifiable
                    // invocations, and none of them should ever be one.
                    return !value.program.empty()
                        && (!value.subcommand || !value.subcommand->empty())
                        && !shell_deletion_denied(value.program)
                        && !shell_launcher_denied(value.program);
                } else if constexpr (std::is_same_v<T, McpGrant>) {
                    return !value.server_id.empty();
                } else if constexpr (std::is_same_v<T, SkillGrant>) {
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

bool grant_covers(
    const PermissionGrant& stored, const PermissionGrant& requested)
{
    if (stored.index() != requested.index()) {
        return false;
    }
    if (const auto* directory = std::get_if<ExternalGrant>(&stored)) {
        return path_within(*directory, std::get<ExternalGrant>(requested));
    }
    if (const auto* command = std::get_if<ShellCommandGrant>(&stored)) {
        const auto& other = std::get<ShellCommandGrant>(requested);
        return command->program == other.program
            && (!command->subcommand
                || command->subcommand == other.subcommand);
    }
    if (const auto* skill = std::get_if<SkillGrant>(&stored)) {
        return *skill == std::get<SkillGrant>(requested);
    }
    return std::get<McpGrant>(stored).server_id
        == std::get<McpGrant>(requested).server_id;
}

bool PermissionStore::install(Grants grants)
{
    if (!std::all_of(grants.begin(), grants.end(), normalize_grant)) {
        return false;
    }
    if (grants.empty()) {
        return true;
    }
    {
        std::lock_guard lock(_mutex);
        if (std::all_of(
                grants.begin(), grants.end(), [&](const auto& requested) {
                    return std::any_of(_grants->begin(), _grants->end(),
                        [&](const auto& stored) {
                            return grant_covers(stored, requested);
                        });
                })) {
            return true;
        }
        Grants next = *_grants;
        for (PermissionGrant& grant : grants) {
            if (std::any_of(next.begin(), next.end(), [&](const auto& stored) {
                    return grant_covers(stored, grant);
                })) {
                continue;
            }
            std::erase_if(next, [&](const auto& stored) {
                return grant_covers(grant, stored);
            });
            next.push_back(std::move(grant));
        }
        _grants = std::make_shared<const Grants>(std::move(next));
    }
    _changed.publish();
    return true;
}

void PermissionStore::clear()
{
    bool changed = false;
    {
        std::lock_guard lock(_mutex);
        changed = !_grants->empty();
        if (changed) {
            _grants = std::make_shared<const Grants>();
        }
    }
    if (changed) {
        _changed.publish();
    }
}

Signal<>::Subscription PermissionStore::subscribe_to_grants_change(
    Signal<>::Callback callback)
{
    return _changed.subscribe(std::move(callback));
}

bool PermissionStore::skip_permissions() const
{
    return _skip_permissions.load(std::memory_order_relaxed);
}

void PermissionStore::set_skip_permissions(bool skip)
{
    if (_skip_permissions.exchange(skip, std::memory_order_relaxed) != skip) {
        _skip_changed.publish();
    }
}

Signal<>::Subscription PermissionStore::subscribe_to_skip_change(
    Signal<>::Callback callback)
{
    return _skip_changed.subscribe(std::move(callback));
}

bool grants_cover(
    const PermissionStore::Grants& grants, const PermissionGrant& requested)
{
    return std::any_of(grants.begin(), grants.end(),
        [&](const auto& stored) { return grant_covers(stored, requested); });
}

PermissionStore::Grants allow_grants(const AllowConfig& allow)
{
    PermissionStore::Grants grants;
    grants.reserve(allow.directories.size() + allow.commands.size());
    std::error_code ec;
    for (const std::filesystem::path& entry : allow.directories) {
        const std::filesystem::path resolved = entry.is_absolute()
            ? entry
            : std::filesystem::weakly_canonical(
                  std::filesystem::current_path() / entry, ec);
        if (ec || !std::filesystem::is_directory(resolved, ec) || ec) {
            continue;
        }
        grants.emplace_back(resolved);
    }
    for (const ShellCommandGrant& command : allow.commands) {
        grants.emplace_back(command);
    }
    return grants;
}

} // namespace imza
