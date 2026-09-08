#include "permissions/store.h"

#include <algorithm>
#include <type_traits>

#include "common/util.h"

namespace ursa {

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
                    return !value.program.empty()
                        && (!value.subcommand || !value.subcommand->empty());
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
    {
        std::lock_guard lock(_mutex);
        if (std::all_of(
                grants.begin(), grants.end(), [&](const auto& requested) {
                    return std::any_of(_grants->begin(), _grants->end(),
                        [&](const auto& stored) {
                            return _covers(stored, requested);
                        });
                })) {
            return true;
        }
        Grants next = *_grants;
        for (PermissionGrant& grant : grants) {
            if (std::any_of(next.begin(), next.end(), [&](const auto& stored) {
                    return _covers(stored, grant);
                })) {
                continue;
            }
            std::erase_if(next,
                [&](const auto& stored) { return _covers(grant, stored); });
            next.push_back(std::move(grant));
        }
        _grants = std::make_shared<const Grants>(std::move(next));
    }
    _changed.publish();
    return true;
}

bool PermissionStore::matches(const ShellCommandGrant& grant) const
{
    const Snapshot grants = snapshot();
    return std::any_of(grants->begin(), grants->end(), [&](const auto& entry) {
        const auto* stored = std::get_if<ShellCommandGrant>(&entry);
        return stored != nullptr && stored->program == grant.program
            && (!stored->subcommand || stored->subcommand == grant.subcommand);
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

bool PermissionStore::_covers(
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
    return std::get<SkillGrant>(stored) == std::get<SkillGrant>(requested);
}

} // namespace ursa
