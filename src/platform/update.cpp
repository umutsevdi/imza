#include "platform/update.h"

#include "network/json.h"
#include "network/network.h"
#include "platform/command_runner.h"
#include "platform/config.h"
#include "platform/json_file.h"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <optional>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace imza {

// Glaze-reflected: must have external linkage (Clang/MSVC requirement).
struct UpdateCache {
    std::int64_t last_checked_at = 0;
    std::string version;
};

// GitHub release payload wire shape.
struct StoredAsset {
    std::optional<std::string> name;
    std::optional<std::string> browser_download_url;
};

struct StoredRelease {
    std::optional<std::string> tag_name;
    std::optional<bool> draft;
    std::optional<bool> prerelease;
    std::vector<StoredAsset> assets;
};

namespace {

    constexpr const char* RELEASES_URL
        = "https://api.github.com/repos/umutsevdi/imza/releases/latest";
    constexpr long FETCH_TIMEOUT_SECS    = 30;
    constexpr long DOWNLOAD_TIMEOUT_SECS = 600;
    constexpr std::int64_t ONE_DAY_SECS  = 86400;

    std::string_view version_core(std::string_view version)
    {
        if (version.starts_with('v')) {
            version.remove_prefix(1);
        }
        if (version.starts_with("imza-")) {
            version.remove_prefix(5);
        }
        const auto cut = version.find('-');
        if (cut != std::string_view::npos) {
            version = version.substr(0, cut);
        }
        return version;
    }

    std::string_view without_leading_v(std::string_view version)
    {
        return version.starts_with('v') ? version.substr(1) : version;
    }

    long long component_value(std::string_view& text)
    {
        long long value = 0;
        std::size_t end = 0;
        while (end < text.size() && text[end] >= '0' && text[end] <= '9') {
            value = value * 10 + (text[end] - '0');
            ++end;
        }
        text = text.substr(end);
        return value;
    }

    bool update_cache_stale(
        const UpdateCache& cache, std::int64_t now_unix_secs)
    {
        if (cache.last_checked_at <= 0) {
            return true;
        }
        return cache.last_checked_at + ONE_DAY_SECS <= now_unix_secs;
    }

    UpdateCache load_update_cache(const std::filesystem::path& path)
    {
        const std::optional<std::string> text = read_text_file(path);
        if (!text) {
            return { };
        }
        UpdateCache cache;
        if (json_parse_checked(*text, cache)) {
            return { };
        }
        return cache;
    }

    Status save_update_cache(
        const std::filesystem::path& path, const UpdateCache& cache)
    {
        auto serialized = json_dump_checked(cache);
        if (!serialized) {
            return Status::JSON_ERROR;
        }
        return write_json_file(path, *serialized);
    }

    std::optional<std::string> cached_version(
        const UpdateCache& cache, std::string_view current_version)
    {
        if (!cache.version.empty()
            && compare_versions(cache.version, current_version) > 0) {
            return cache.version;
        }
        return std::nullopt;
    }

    std::optional<UpdateInfo> parse_latest_release(std::string_view json_body,
        const std::vector<std::string>& package_managers,
        std::string_view current_version)
    {
        StoredRelease release;
        if (json_parse_checked(json_body, release) || !release.tag_name) {
            return std::nullopt;
        }
        if (release.draft.value_or(false)
            || release.prerelease.value_or(false)) {
            return std::nullopt;
        }
        const std::string version(without_leading_v(*release.tag_name));
        if (compare_versions(version, current_version) <= 0) {
            return std::nullopt;
        }
        const std::string asset
            = expected_asset_name(package_managers, version);
        if (asset.empty()) {
            return std::nullopt;
        }
        for (const auto& entry : release.assets) {
            if (entry.name && *entry.name == asset
                && entry.browser_download_url) {
                return UpdateInfo { version, *entry.browser_download_url };
            }
        }
        return std::nullopt;
    }

} // namespace

int compare_versions(std::string_view a, std::string_view b)
{
    std::string_view left  = version_core(a);
    std::string_view right = version_core(b);
    while (!left.empty() || !right.empty()) {
        const long long x = component_value(left);
        const long long y = component_value(right);
        if (x != y) {
            return x < y ? -1 : 1;
        }
        if (!left.empty() && left.front() == '.') {
            left.remove_prefix(1);
        }
        if (!right.empty() && right.front() == '.') {
            right.remove_prefix(1);
        }
    }
    return 0;
}

std::string expected_asset_name(
    const std::vector<std::string>& package_managers, std::string_view version)
{
    const std::string v(without_leading_v(version));
#if defined(__x86_64__) || defined(__x86_64) || defined(_M_X64)
    const char* arch = "x64";
#elif defined(__aarch64__) || defined(__aarch64) || defined(_M_ARM64)
    const char* arch = "arm64";
#else
    (void)package_managers;
    (void)v;
    return "";
#endif
#if defined(_WIN32)
    (void)package_managers;
    const char* os  = "windows";
    const char* ext = "exe";
#elif defined(__APPLE__)
    (void)package_managers;
    const char* os  = "macos";
    const char* ext = "pkg";
#else
    const char* os  = "linux";
    const char* ext = nullptr;
    if (std::ranges::contains(package_managers, "apt")
        || std::ranges::contains(package_managers, "apt-get")) {
        ext = "deb";
    } else if (std::ranges::contains(package_managers, "dnf")
        || std::ranges::contains(package_managers, "yum")) {
        ext = "rpm";
    }
    if (ext == nullptr) {
        (void)v;
        return "";
    }
#endif
    return std::string("imza-") + v + "-" + os + "-" + arch + "." + ext;
}

std::optional<std::string> cached_update(std::string_view current_version)
{
    return cached_version(
        load_update_cache(update_state_path()), current_version);
}

std::optional<std::string> check_for_update(
    const std::vector<std::string>& package_managers,
    std::string_view current_version)
{
    const UpdateCache cache = load_update_cache(update_state_path());
    if (!update_cache_stale(cache, std::time(nullptr))) {
        return cached_version(cache, current_version);
    }
    const std::optional<UpdateInfo> update
        = fetch_update(package_managers, current_version);
    return update ? std::optional<std::string>(update->version) : std::nullopt;
}

std::optional<UpdateInfo> fetch_update(
    const std::vector<std::string>& package_managers,
    std::string_view current_version)
{
    std::string body;
    long http_code      = 0;
    const Status status = http_get(RELEASES_URL,
        { "User-Agent: imza-updater", "Accept: application/vnd.github+json" },
        FETCH_TIMEOUT_SECS, body, &http_code);
    const std::optional<UpdateInfo> update
        = status == Status::OK && http_ok(http_code)
        ? parse_latest_release(body, package_managers, current_version)
        : std::nullopt;
    save_update_cache(update_state_path(),
        UpdateCache { std::time(nullptr), update ? update->version : "" });
    return update;
}

int install_update(
    const UpdateInfo& update, const std::filesystem::path& directory)
{
    const auto slash = update.download_url.find_last_of('/');
    if (slash == std::string::npos || slash + 1 >= update.download_url.size()) {
        return 1;
    }
    std::string body;
    long http_code = 0;
    const Status status
        = http_get(update.download_url, { "User-Agent: imza-updater" },
            DOWNLOAD_TIMEOUT_SECS, body, &http_code);
    if (status != Status::OK || !http_ok(http_code)) {
        return 1;
    }
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    const std::filesystem::path installer
        = directory / update.download_url.substr(slash + 1);
    std::ofstream file(installer, std::ios::binary | std::ios::trunc);
    if (!file) {
        return 1;
    }
    file.write(body.data(), static_cast<std::streamsize>(body.size()));
    file.close();
    if (!file.good()) {
        return 1;
    }
#if defined(_WIN32)
    const CommandResult result = run_attached_command(shell_quote(installer)
        + " /SILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS");
    return result.spawned ? result.exit_code : 1;
#elif defined(__APPLE__)
    return open_browser(installer.string()) ? 0 : 1;
#else
    std::string command;
    if (geteuid() != 0) {
        command = "sudo -- ";
    }
    command += installer.extension() == ".deb" ? "apt" : "dnf";
    command += " install -y ";
    command += shell_quote(installer);
    const CommandResult result = run_attached_command(command);
    return result.spawned ? result.exit_code : 1;
#endif
}

} // namespace imza
