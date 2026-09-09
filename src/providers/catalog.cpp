#include "providers/catalog.h"
#include "network/json_io.h"
#include "platform/json_file.h"

#include <algorithm>
#include <array>
#include <ctime>
#include <fstream>
#include <sstream>

#include "common/util.h"

namespace imza {

namespace {

    constexpr std::string_view CATALOG_URL  = "https://models.dev/api.json";
    constexpr long FETCH_TIMEOUT_SECS       = 60;
    constexpr std::int64_t STALE_AFTER_SECS = 7 * 24 * 3600;

    constexpr std::array<std::string_view, 51> WHITELIST = {
        "abacus",
        "alibaba",
        "alibaba-cn",
        "alibaba-coding-plan",
        "alibaba-coding-plan-cn",
        "alibaba-token-plan",
        "alibaba-token-plan-cn",
        "amd",
        "anthropic",
        "cerebras",
        "databricks",
        "deepseek",
        "digitalocean",
        "github-copilot",
        "groq",
        "hetzner",
        "huggingface",
        "hyper",
        "kilo",
        "kimi-for-coding",
        "llama",
        "llmgateway",
        "llmgateway-providers",
        "llmtr",
        "meta",
        "minimax",
        "mistral",
        "moonshotai",
        "moonshotai-cn",
        "nebius",
        "nvidia",
        "ollama-cloud",
        "openai",
        "opencode",
        "opencode-go",
        "openrouter",
        "perplexity-agent",
        "tencent-coding-plan",
        "tencent-token-plan",
        "tencent-tokenhub",
        "thinkingmachines",
        "togetherai",
        "vultr",
        "xai",
        "xiaomi-token-plan-ams",
        "xiaomi-token-plan-cn",
        "xiaomi-token-plan-sgp",
        "zai",
        "zai-coding-plan",
        "zhipuai",
        "zhipuai-coding-plan",
    };

    const std::map<std::pair<std::string, ApiStandard>, std::string>
        PROVIDER_URLS = {
            { { "anthropic", ApiStandard::ANTHROPIC },
                "https://api.anthropic.com/v1" },
            { { "cerebras", ApiStandard::OPENAI },
                "https://api.cerebras.ai/v1" },
            { { "groq", ApiStandard::OPENAI },
                "https://api.groq.com/openai/v1" },
            { { "mistral", ApiStandard::OPENAI }, "https://api.mistral.ai/v1" },
            { { "openai", ApiStandard::OPENAI_RESPONSES },
                "https://api.openai.com/v1" },
            { { "togetherai", ApiStandard::OPENAI },
                "https://api.together.xyz/v1" },
            { { "xai", ApiStandard::OPENAI }, "https://api.x.ai/v1" },
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
        return !conn.endpoint.empty();
    }

    constexpr std::string_view CHAT_SUFFIX      = "/chat/completions";
    constexpr std::string_view RESPONSES_SUFFIX = "/responses";

    std::string normalize_base(std::string_view base)
    {
        std::string out = strip_slash(base);
        for (std::string_view suffix : { CHAT_SUFFIX, RESPONSES_SUFFIX }) {
            if (out.size() > suffix.size()
                && std::string_view(out).substr(out.size() - suffix.size())
                    == suffix) {
                out.resize(out.size() - suffix.size());
                break;
            }
        }
        return out;
    }

    bool whitelisted_provider(std::string_view id)
    {
        return std::find(WHITELIST.begin(), WHITELIST.end(), id)
            != WHITELIST.end();
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
                model.cost_input       = cost_field(cost, "input");
                model.cost_output      = cost_field(cost, "output");
                model.cost_cache_read  = cost_field(cost, "cache_read");
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

} // namespace

bool catalog_stale(const Catalog& catalog)
{
    if (catalog.fetched_at <= 0) {
        return true;
    }
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    return now - catalog.fetched_at > STALE_AFTER_SECS;
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
            if (model.cost_input || model.cost_output || model.cost_cache_read
                || model.cost_cache_write) {
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

    return write_json_file(path, root, "  ");
}

Status fetch_catalog(Catalog& out)
{
    std::string body;
    long code       = 0;
    const Status st = http_get(
        std::string(CATALOG_URL), { }, FETCH_TIMEOUT_SECS, body, &code);
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
    backfill_catalog_urls(catalog);
    catalog.fetched_at = static_cast<std::int64_t>(std::time(nullptr));
    out                = std::move(catalog);
    return Status::OK;
}

void backfill_catalog_urls(Catalog& catalog)
{
    for (auto& [id, provider] : catalog.providers) {
        if (!provider.api.empty()) {
            continue;
        }
        const auto url
            = PROVIDER_URLS.find({ id, dialect_from_npm(provider.npm) });
        if (url != PROVIDER_URLS.end()) {
            provider.api = url->second;
        }
    }
    inject_subscription_providers(catalog);
}

void inject_subscription_providers(Catalog& catalog)
{
    CachedProvider openai;
    openai.name = "Open AI Subscription";
    openai.api  = "https://chatgpt.com/backend-api/codex";
    openai.npm  = "@ai-sdk/openai";
    for (std::string_view id :
        { "gpt-5.6-sol", "gpt-5.6-terra", "gpt-5.6-luna", "gpt-5.5" }) {
        CachedModel model;
        model.name      = std::string(id);
        model.tool_call = true;
        model.reasoning = true;
        openai.models.emplace(std::string(id), std::move(model));
    }
    catalog.providers[std::string(OPENAI_SUBSCRIPTION_ID)] = std::move(openai);

    CachedProvider anthropic;
    anthropic.name = "Anthropic Subscription";
    anthropic.api  = "https://api.anthropic.com/v1";
    anthropic.npm  = "@ai-sdk/anthropic";
    if (const auto source = catalog.providers.find("anthropic");
        source != catalog.providers.end()) {
        anthropic.models = source->second.models;
    }
    catalog.providers[std::string(ANTHROPIC_SUBSCRIPTION_ID)]
        = std::move(anthropic);
}

AuthType auth_from_npm(std::string_view npm)
{
    return npm.find("anthropic") != std::string_view::npos ? AuthType::ANTHROPIC
                                                           : AuthType::BEARER;
}

ApiStandard dialect_from_npm(std::string_view npm)
{
    if (npm == "@ai-sdk/openai") {
        return ApiStandard::OPENAI_RESPONSES;
    }
    if (npm == "@ai-sdk/anthropic") {
        return ApiStandard::ANTHROPIC;
    }
    return ApiStandard::OPENAI;
}

std::string catalog_base(const CachedProvider& provider)
{
    return strip_slash(provider.api);
}

Route resolve_route(
    const Connection& conn, const Catalog& catalog, ApiStandard dialect)
{
    Route route;
    route.api_key    = conn.api_key;
    route.account_id = conn.account_id;

    if (endpoint_backed(conn)) {
        route.api = normalize_base(conn.endpoint);
        if (dialect == ApiStandard::OPENAI_RESPONSES
            || std::string_view(conn.endpoint).ends_with(RESPONSES_SUFFIX)) {
            route.endpoint = route.api + std::string(RESPONSES_SUFFIX);
            route.dialect  = ApiStandard::OPENAI_RESPONSES;
        } else {
            route.endpoint = conn.endpoint;
            route.dialect  = ApiStandard::OPENAI;
        }
        route.auth = conn.api_key.empty() ? AuthType::NONE : AuthType::BEARER;
        return route;
    }

    if (conn.id == OPENAI_SUBSCRIPTION_ID) {
        route.api      = "https://chatgpt.com/backend-api/codex";
        route.endpoint = route.api + "/responses";
        route.dialect  = ApiStandard::OPENAI_RESPONSES;
        route.auth     = AuthType::OPENAI_SUBSCRIPTION;
        return route;
    }
    if (conn.id == ANTHROPIC_SUBSCRIPTION_ID) {
        route.api      = "https://api.anthropic.com/v1";
        route.endpoint = route.api + "/messages";
        route.dialect  = ApiStandard::ANTHROPIC;
        route.auth     = AuthType::ANTHROPIC_SUBSCRIPTION;
        return route;
    }

    const auto it = catalog.providers.find(conn.id);
    if (it == catalog.providers.end()) {
        return route;
    }
    const CachedProvider& provider = it->second;
    route.api                      = catalog_base(provider);
    if (route.api.empty()) {
        return route;
    }
    route.dialect = dialect;
    route.auth
        = conn.api_key.empty() ? AuthType::NONE : auth_from_npm(provider.npm);
    switch (dialect) {
    case ApiStandard::OPENAI:
        route.endpoint = route.api + "/chat/completions";
        break;
    case ApiStandard::OPENAI_RESPONSES:
        route.endpoint = route.api + "/responses";
        break;
    case ApiStandard::ANTHROPIC:
        route.endpoint = route.api + "/messages";
        break;
    }
    return route;
}

std::string endpoint_for_base(std::string_view base)
{
    if (base.empty()) {
        return { };
    }
    return normalize_base(base) + std::string(CHAT_SUFFIX);
}

} // namespace imza
