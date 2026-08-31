#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <json/json.h>

#include "network.h"
#include "types.h"

namespace ursa {

struct CachedModel {
    std::string name;
    std::optional<double> cost_input;
    std::optional<double> cost_output;
    std::optional<double> cost_cache_read;
    std::optional<double> cost_cache_write;
    std::optional<std::uint64_t> context;
    std::optional<std::uint64_t> output;
    std::optional<bool> tool_call;
    std::optional<bool> reasoning;
};

struct CachedProvider {
    std::string name;
    std::string api;
    std::string npm;
    std::map<std::string, CachedModel> models;
};

struct Catalog {
    std::int64_t fetched_at = 0;
    std::map<std::string, CachedProvider> providers;
};

inline constexpr std::string_view kLocalProviderId  = "local";
inline constexpr std::string_view kCustomProviderId = "custom";

bool catalog_stale(const Catalog& catalog);
Status load_catalog(const std::filesystem::path& path, Catalog& out);
Status save_catalog(const std::filesystem::path& path, const Catalog& catalog);
Status fetch_catalog(Catalog& out);
bool whitelisted_provider(std::string_view id);
Status trim_provider(const Json::Value& src, CachedProvider& out);

AuthType auth_from_npm(std::string_view npm);
std::string catalog_base(const CachedProvider& provider);
Route resolve_route(
    const Connection& conn, const Catalog& catalog, ApiStandard dialect);

} // namespace ursa
