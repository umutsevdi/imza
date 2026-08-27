#include "catalog.h"

#include <algorithm>
#include <array>
#include <ctime>
#include <fstream>
#include <sstream>

#include "util.h"

namespace ursa {

namespace {

    constexpr std::string_view kCatalogUrl = "https://models.dev/api.json";
    constexpr long kFetchTimeoutSecs = 60;
    constexpr std::int64_t kStaleAfterSecs = 7 * 24 * 3600;

    constexpr std::array<std::string_view, 20> kWhitelist = {
        "openai", "anthropic", "zai", "zai-coding-plan", "zhipuai",
        "openrouter", "groq", "mistral", "deepseek", "moonshotai", "xai",
        "opencode", "google", "minimax", "fireworks", "together-ai",
        "cerebras", "perplexity", "azure", "amazon-bedrock"
    };

    std::optional<double> cost_field(const Json::Value& cost, const char* key)
    {
        const Json::Value& v = cost[key];
        if (!v.isNumeric()) {
            return std::nullopt;
        }
        return v.asDouble();
    }

    std::optional<std::uint64_t> limit_field(
        const Json::Value& limit, const char* key)
    {
        const Json::Value& v = limit[key];
        if (!v.isUInt64()) {
            return std::nullopt;
        }
        return v.asUInt64();
    }

    bool endpoint_backed(const Connection& conn)
    {
        return conn.provider_id == kLocalProviderId
            || conn.provider_id == kCustomProviderId || !conn.endpoint.empty();
    }

} // namespace

bool whitelisted_provider(std::string_view id)
{
    return std::find(kWhitelist.begin(), kWhitelist.end(), id)
        != kWhitelist.end();
}

Status trim_provider(const Json::Value& src, CachedProvider& out)
{
    if (!src.isObject()) {
        return Status::JSON_ERROR;
    }
    if (src["name"].isString()) {
        out.name = src["name"].asString();
    }
    if (src["api"].isString()) {
        out.api = src["api"].asString();
    }
    if (src["npm"].isString()) {
        out.npm = src["npm"].asString();
    }
    const Json::Value& models = src["models"];
    if (models.isNull()) {
        return Status::OK;
    }
    if (!models.isObject()) {
        return Status::JSON_ERROR;
    }
    for (const std::string& id : models.getMemberNames()) {
        const Json::Value& entry = models[id];
        if (!entry.isObject()) {
            continue;
        }
        CachedModel model;
        if (entry["name"].isString()) {
            model.name = entry["name"].asString();
        }
        const Json::Value& cost = entry["cost"];
        if (cost.isObject()) {
            model.cost_input      = cost_field(cost, "input");
            model.cost_output     = cost_field(cost, "output");
            model.cost_cache_read = cost_field(cost, "cache_read");
            model.cost_cache_write = cost_field(cost, "cache_write");
        }
        const Json::Value& limit = entry["limit"];
        if (limit.isObject()) {
            model.context = limit_field(limit, "context");
            model.output  = limit_field(limit, "output");
        }
        if (entry["tool_call"].isBool()) {
            model.tool_call = entry["tool_call"].asBool();
        }
        if (entry["reasoning"].isBool()) {
            model.reasoning = entry["reasoning"].asBool();
        }
        out.models[id] = std::move(model);
    }
    return Status::OK;
}

bool catalog_stale(const Catalog& catalog)
{
    if (catalog.fetched_at <= 0) {
        return true;
    }
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    return now - catalog.fetched_at > kStaleAfterSecs;
}

Status load_catalog(const std::filesystem::path& path, Catalog& out)
{
    out = Catalog { };

    std::ifstream file(path);
    if (!file) {
        return Status::OK;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();

    const Json::Value root = parse_json(buffer.str());
    if (root.isNull() || !root.isObject()) {
        return Status::JSON_ERROR;
    }
    if (root["fetched_at"].isInt64()) {
        out.fetched_at = root["fetched_at"].asInt64();
    }
    const Json::Value& providers = root["providers"];
    if (providers.isNull()) {
        return Status::OK;
    }
    if (!providers.isObject()) {
        return Status::JSON_ERROR;
    }
    for (const std::string& id : providers.getMemberNames()) {
        CachedProvider provider;
        const Status st = trim_provider(providers[id], provider);
        if (st != Status::OK) {
            return st;
        }
        out.providers[id] = std::move(provider);
    }
    return Status::OK;
}

Status save_catalog(const std::filesystem::path& path, const Catalog& catalog)
{
    Json::Value root(Json::objectValue);
    root["fetched_at"] = catalog.fetched_at;

    Json::Value providers(Json::objectValue);
    for (const auto& [id, provider] : catalog.providers) {
        Json::Value entry(Json::objectValue);
        entry["name"] = provider.name;
        if (!provider.api.empty()) {
            entry["api"] = provider.api;
        }
        if (!provider.npm.empty()) {
            entry["npm"] = provider.npm;
        }
        Json::Value models(Json::objectValue);
        for (const auto& [id, model] : provider.models) {
            Json::Value entry(Json::objectValue);
            if (!model.name.empty()) {
                entry["name"] = model.name;
            }
            if (model.cost_input || model.cost_output
                || model.cost_cache_read || model.cost_cache_write) {
                Json::Value cost(Json::objectValue);
                if (model.cost_input) {
                    cost["input"] = *model.cost_input;
                }
                if (model.cost_output) {
                    cost["output"] = *model.cost_output;
                }
                if (model.cost_cache_read) {
                    cost["cache_read"] = *model.cost_cache_read;
                }
                if (model.cost_cache_write) {
                    cost["cache_write"] = *model.cost_cache_write;
                }
                entry["cost"] = cost;
            }
            if (model.context || model.output) {
                Json::Value limit(Json::objectValue);
                if (model.context) {
                    limit["context"] = *model.context;
                }
                if (model.output) {
                    limit["output"] = *model.output;
                }
                entry["limit"] = limit;
            }
            if (model.tool_call) {
                entry["tool_call"] = *model.tool_call;
            }
            if (model.reasoning) {
                entry["reasoning"] = *model.reasoning;
            }
            models[id] = entry;
        }
        entry["models"] = models;
        providers[id]   = entry;
    }
    root["providers"] = providers;

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream file(tmp, std::ios::trunc);
        if (!file) {
            return Status::CONFIG_ERROR;
        }
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "  ";
        file << Json::writeString(builder, root) << "\n";
        if (!file) {
            return Status::CONFIG_ERROR;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        return Status::CONFIG_ERROR;
    }
    return Status::OK;
}

Status fetch_catalog(Catalog& out)
{
    std::string body;
    long code = 0;
    const Status st
        = http_get(std::string(kCatalogUrl), { }, kFetchTimeoutSecs, body,
            &code);
    if (st != Status::OK) {
        return st;
    }
    if (code < 200 || code >= 300) {
        return Status::API_ERROR;
    }

    const Json::Value root = parse_json(body);
    if (root.isNull() || !root.isObject()) {
        return Status::JSON_ERROR;
    }

    Catalog catalog;
    for (const std::string& id : root.getMemberNames()) {
        if (!whitelisted_provider(id)) {
            continue;
        }
        CachedProvider provider;
        const Status st = trim_provider(root[id], provider);
        if (st != Status::OK) {
            continue;
        }
        catalog.providers[id] = std::move(provider);
    }
    catalog.fetched_at = static_cast<std::int64_t>(std::time(nullptr));
    out                = std::move(catalog);
    return Status::OK;
}

AuthType auth_from_npm(std::string_view npm)
{
    return npm.find("anthropic") != std::string_view::npos
        ? AuthType::ANTHROPIC
        : AuthType::BEARER;
}

std::string catalog_base(const CachedProvider& provider)
{
    if (!provider.api.empty()) {
        return strip_slash(provider.api);
    }
    if (provider.npm.find("anthropic") != std::string::npos) {
        return "https://api.anthropic.com/v1";
    }
    if (provider.npm.find("@ai-sdk/openai") != std::string::npos) {
        return "https://api.openai.com/v1";
    }
    return { };
}

Route resolve_route(const Connection& conn, const Catalog& catalog,
    ApiStandard dialect)
{
    Route route;
    route.api_key = conn.api_key;

    if (endpoint_backed(conn)) {
        route.endpoint = conn.endpoint;
        route.api      = strip_slash(conn.endpoint);
        constexpr std::string_view kSuffix = "/chat/completions";
        if (route.api.size() > kSuffix.size()
            && std::string_view(route.api).substr(
                   route.api.size() - kSuffix.size())
                == kSuffix) {
            route.api.resize(route.api.size() - kSuffix.size());
        }
        route.dialect = ApiStandard::OPENAI;
        route.auth    = conn.api_key.empty() ? AuthType::NONE
                                             : AuthType::BEARER;
        return route;
    }

    const auto it = catalog.providers.find(conn.provider_id);
    if (it == catalog.providers.end()) {
        return route;
    }
    const CachedProvider& provider = it->second;
    route.api                      = catalog_base(provider);
    if (route.api.empty()) {
        return route;
    }
    route.dialect = dialect;
    route.auth    = conn.api_key.empty() ? AuthType::NONE
                                         : auth_from_npm(provider.npm);
    route.endpoint = route.api
        + (dialect == ApiStandard::ANTHROPIC ? "/messages"
                                             : "/chat/completions");
    return route;
}

CachedProvider local_provider()
{
    CachedProvider provider;
    provider.name = "Local";
    provider.api  = "http://localhost:11434/v1";
    provider.npm  = "@ai-sdk/openai-compatible";
    return provider;
}

} // namespace ursa
