#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "common/types.h"
#include "network/network.h"
#include "platform/config.h"

namespace imza {

struct CachedModel {
    std::string name;
    std::optional<double> cost_input;
    std::optional<double> cost_output;
    std::optional<double> cost_cache_read;
    std::optional<double> cost_cache_write;
    std::optional<std::uint64_t> context;
    std::optional<bool> tool_call;
    std::optional<bool> reasoning;
    std::optional<Capabilities> capabilities;
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

inline constexpr std::string_view CUSTOM_PROVIDER_ID  = "custom";
inline constexpr std::string_view EVREN_PROVIDER_ID   = "evren";
inline constexpr std::string_view EVREN_PROVIDER_NAME = "EVREN";
// OPENAI_SUBSCRIPTION_ID lives in common/types.h: the credential shape is a
// platform concern and must not reach into the provider catalogue.
inline constexpr std::string_view OPENAI_SUBSCRIPTION_NAME
    = "Open AI Subscription";

bool catalog_stale(const Catalog& catalog);
Status load_catalog(const std::filesystem::path& path, Catalog& out);
Status save_catalog(const std::filesystem::path& path, const Catalog& catalog);
Status fetch_catalog(Catalog& out);
void backfill_catalog_urls(Catalog& catalog);
void inject_subscription_providers(Catalog& catalog);
// Injects providers that models.dev does not list (subscriptions and EVREN).
void inject_local_providers(Catalog& catalog);

AuthType auth_from_npm(std::string_view npm);
ApiStandard dialect_from_npm(std::string_view npm);
// Base URL for a catalog provider, derived from the environment when the
// provider's endpoint depends on the user's cloud tenancy. Empty when a
// required variable is unset, with `missing` naming it.
std::string catalog_base(const CachedProvider& provider,
    std::string_view provider_id = { }, std::string* missing = nullptr);
// Whether the provider lists models from the catalog instead of probing.
bool catalog_models_only(std::string_view provider_id);
Route resolve_route(const Connection& conn, const Catalog& catalog,
    ApiStandard dialect, std::string_view opencode_session = { });
std::string endpoint_for_base(std::string_view base);

} // namespace imza
