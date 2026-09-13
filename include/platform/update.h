#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/types.h"

namespace imza {

struct UpdateInfo {
    std::string version;
    std::string download_url;
};

// Negative when `a` is older than `b`. Tolerates a leading `v`, an
// `imza-` prefix, and a trailing package revision (`0.3.1-1`).
int compare_versions(std::string_view a, std::string_view b);

// Release asset this build can install, or empty when the platform has no
// artifact for its package managers or architecture.
std::string expected_asset_name(
    const std::vector<std::string>& package_managers, std::string_view version);

// Newer version recorded in update.json, or nullopt. Network-free.
std::optional<std::string> cached_update(std::string_view current_version);

// Like cached_update, but refetches GitHub and persists the result when
// update.json is older than a day.
std::optional<std::string> check_for_update(
    const std::vector<std::string>& package_managers,
    std::string_view current_version);

// Fetches the latest release regardless of the cache, persists it, and
// returns the installable update.
std::optional<UpdateInfo> fetch_update(
    const std::vector<std::string>& package_managers,
    std::string_view current_version);

// Downloads the release asset into `directory` and runs its installer;
// returns the installer exit status.
int install_update(
    const UpdateInfo& update, const std::filesystem::path& directory);

} // namespace imza
