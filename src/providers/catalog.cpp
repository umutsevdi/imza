#include "providers/catalog.h"
#include "network/json.h"
#include "platform/json_file.h"

#include <algorithm>
#include <array>
#include <ctime>
#include <optional>

#include "common/util.h"

namespace imza {

namespace {

    constexpr std::string_view CATALOG_URL  = "https://models.dev/api.json";
    constexpr long FETCH_TIMEOUT_SECS       = 60;
    constexpr std::int64_t STALE_AFTER_SECS = 7 * 24 * 3600;

    constexpr std::array<std::string_view, 57> WHITELIST = {
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
        "deepinfra",
        "deepseek",
        "digitalocean",
        "fireworks-ai",
        "github-copilot",
        "google",
        "groq",
        "hetzner",
        "huggingface",
        "hyper",
        "kilo",
        "kimi-code-plan-cn",
        "kimi-code-plan-global",
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
        "siliconflow",
        "tencent-coding-plan",
        "tencent-token-plan",
        "tencent-tokenhub",
        "thinkingmachines",
        "togetherai",
        "vultr",
        "wandb",
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
            { { "deepinfra", ApiStandard::OPENAI },
                "https://api.deepinfra.com/v1/openai" },
            { { "google", ApiStandard::OPENAI },
                "https://generativelanguage.googleapis.com/v1beta/openai" },
            { { "groq", ApiStandard::OPENAI },
                "https://api.groq.com/openai/v1" },
            { { "mistral", ApiStandard::OPENAI }, "https://api.mistral.ai/v1" },
            { { "openai", ApiStandard::OPENAI_RESPONSES },
                "https://api.openai.com/v1" },
            { { "togetherai", ApiStandard::OPENAI },
                "https://api.together.xyz/v1" },
            { { "xai", ApiStandard::OPENAI }, "https://api.x.ai/v1" },
        };

    // One input modality from models.dev's modalities.input array. Only
    // image/pdf are meaningful today; unknown entries are ignored.
    std::optional<Capabilities> modality_capability(std::string_view name)
    {
        if (name == "image") {
            return Capabilities::IMAGE;
        }
        if (name == "pdf") {
            return Capabilities::PDF;
        }
        return std::nullopt;
    }

    Capabilities capabilities_from_modalities(
        const std::optional<std::vector<std::string>>& input)
    {
        Capabilities capabilities = Capabilities::NONE;
        if (!input) {
            return capabilities;
        }
        for (const std::string& modality : *input) {
            if (const auto flag = modality_capability(modality)) {
                capabilities = capabilities | *flag;
            }
        }
        return capabilities;
    }

    // models.dev entries carry many more fields than imza keeps; the view
    // structs name only the ones that survive into the catalog. Glaze's
    // unknown-key skipping handles the rest.
    struct ModelCostView {
        std::optional<double> input;
        std::optional<double> output;
        std::optional<double> cache_read;
        std::optional<double> cache_write;
    };

    struct ModelLimitView {
        std::optional<std::uint64_t> context;
    };

    struct ModelModalitiesView {
        std::optional<std::vector<std::string>> input;
    };

    struct ProviderModelView {
        std::optional<std::string> name;
        std::optional<ModelCostView> cost;
        std::optional<ModelLimitView> limit;
        std::optional<bool> tool_call;
        std::optional<bool> reasoning;
        std::optional<ModelModalitiesView> modalities;
    };

    struct ProviderView {
        std::optional<std::string> name;
        std::optional<std::string> api;
        std::optional<std::string> npm;
        std::map<std::string, ProviderModelView> models;
    };

    CachedModel to_model(const ProviderModelView& view)
    {
        CachedModel model;
        if (view.name) {
            model.name = *view.name;
        }
        if (view.cost) {
            model.cost_input       = view.cost->input;
            model.cost_output      = view.cost->output;
            model.cost_cache_read  = view.cost->cache_read;
            model.cost_cache_write = view.cost->cache_write;
        }
        if (view.limit) {
            model.context = view.limit->context;
        }
        model.tool_call    = view.tool_call;
        model.reasoning    = view.reasoning;
        model.capabilities = capabilities_from_modalities(
            view.modalities ? view.modalities->input : std::nullopt);
        return model;
    }

    CachedProvider to_provider(const ProviderView& view)
    {
        CachedProvider provider;
        if (view.name) {
            provider.name = *view.name;
        }
        if (view.api) {
            provider.api = *view.api;
        }
        if (view.npm) {
            provider.npm = *view.npm;
        }
        for (const auto& [id, model] : view.models) {
            provider.models[id] = to_model(model);
        }
        return provider;
    }

    // File shape of the cached catalog, reusing the models.dev view
    // structs since the wire formats coincide. Optional members omit when
    // empty; cost/limit objects appear only when they carry a value.
    struct StoredCatalog {
        std::optional<std::int64_t> fetched_at;
        std::map<std::string, ProviderView> providers;
    };

    ProviderModelView to_stored(const CachedModel& model)
    {
        ProviderModelView stored;
        if (!model.name.empty()) {
            stored.name = model.name;
        }
        if (model.cost_input || model.cost_output || model.cost_cache_read
            || model.cost_cache_write) {
            stored.cost = ModelCostView { model.cost_input, model.cost_output,
                model.cost_cache_read, model.cost_cache_write };
        }
        if (model.context) {
            stored.limit = ModelLimitView { model.context };
        }
        stored.tool_call = model.tool_call;
        stored.reasoning = model.reasoning;
        if (model.capabilities) {
            std::vector<std::string> input;
            if (has_capability(*model.capabilities, Capabilities::IMAGE)) {
                input.push_back("image");
            }
            if (has_capability(*model.capabilities, Capabilities::PDF)) {
                input.push_back("pdf");
            }
            stored.modalities = ModelModalitiesView { std::move(input) };
        }
        return stored;
    }

    bool endpoint_backed(const Connection& conn)
    {
        return !conn.endpoint.empty();
    }

    constexpr std::string_view CHAT_SUFFIX      = "/chat/completions";
    constexpr std::string_view RESPONSES_SUFFIX = "/responses";
    constexpr std::string_view MESSAGES_SUFFIX  = "/messages";

    std::string normalize_base(std::string_view base)
    {
        std::string out = strip_slash(base);
        for (std::string_view suffix :
            { CHAT_SUFFIX, RESPONSES_SUFFIX, MESSAGES_SUFFIX }) {
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

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return Status::OK;
    }
    const std::optional<std::string> text = read_text_file(path);
    if (!text) {
        return Status::JSON_ERROR;
    }

    // Stored shape mirrors the file exactly; unknown keys keep files
    // written by other versions readable.
    StoredCatalog stored;
    if (!json_parse(*text, stored)) {
        return Status::JSON_ERROR;
    }
    out.fetched_at = stored.fetched_at.value_or(0);
    for (const auto& [id, provider] : stored.providers) {
        out.providers[id] = to_provider(provider);
    }
    return Status::OK;
}

Status save_catalog(const std::filesystem::path& path, const Catalog& catalog)
{
    std::map<std::string, ProviderView> providers;
    for (const auto& [id, provider] : catalog.providers) {
        ProviderView stored;
        stored.name = provider.name;
        if (!provider.api.empty()) {
            stored.api = provider.api;
        }
        if (!provider.npm.empty()) {
            stored.npm = provider.npm;
        }
        for (const auto& [model_id, model] : provider.models) {
            stored.models[model_id] = to_stored(model);
        }
        providers[id] = std::move(stored);
    }
    const StoredCatalog root { catalog.fetched_at, std::move(providers) };
    auto serialized = json_dump_pretty_checked(root);
    if (!serialized) {
        return Status::JSON_ERROR;
    }
    return write_json_file(path, *serialized);
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
    if (!http_ok(code)) {
        return Status::API_ERROR;
    }

    std::map<std::string, ProviderView> parsed;
    if (!json_parse(body, parsed)) {
        return Status::JSON_ERROR;
    }
    Catalog catalog;
    for (const auto& [id, view] : parsed) {
        if (!whitelisted_provider(id)) {
            continue;
        }
        catalog.providers[id] = to_provider(view);
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
    inject_local_providers(catalog);
}

void inject_local_providers(Catalog& catalog)
{
    inject_subscription_providers(catalog);
    CachedProvider evren;
    evren.name = std::string(EVREN_PROVIDER_NAME);
    evren.api  = "https://evren-llmapi.ssyz.org.tr/v1";
    evren.npm  = "@ai-sdk/openai-compatible";
    catalog.providers[std::string(EVREN_PROVIDER_ID)] = std::move(evren);
}

void inject_subscription_providers(Catalog& catalog)
{
    CachedProvider openai;
    openai.name = std::string(OPENAI_SUBSCRIPTION_NAME);
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

namespace {

    // Coding-plan endpoints authenticate on the client identity in the
    // User-Agent; present an approved tool's identity until imza is
    // recognized, keeping traffic attributable rather than SDK-like.
    struct Disguise {
        std::string_view prefix;
        std::string_view ua;
    };
    constexpr std::array<Disguise, 3> DISGUISE_LIST = {
        { { "zai", "Pi/3.1.0" }, { "zhipuai", "Pi/3.1.0" },
            { "kimi-code-plan", "hermes-agent/1.0" } },
    };

    std::string client_user_agent(std::string_view provider_id)
    {
        if (provider_id.starts_with(OPENAI_SUBSCRIPTION_ID)) {
            return { };
        }
        for (const Disguise& disguise : DISGUISE_LIST) {
            if (provider_id.starts_with(disguise.prefix)) {
                return "User-Agent: " + std::string(disguise.ua);
            }
        }
        return "User-Agent: imza/" IMZA_VERSION;
    }

    // OpenCode Go routes through opencode.ai/zen/go and expects third-party
    // agents to carry a stable per-conversation id in `x-opencode-session`;
    // without it the gateway classifies the client as anonymous.
    bool opencode_gateway(std::string_view provider_id)
    {
        return provider_id.starts_with("opencode");
    }

} // namespace

Route resolve_route(const Connection& conn, const Catalog& catalog,
    ApiStandard dialect, std::string_view opencode_session)
{
    Route route;
    route.user_agent = client_user_agent(conn.id);
    if (opencode_gateway(conn.id) && !opencode_session.empty()) {
        route.opencode_session = std::string(opencode_session);
    }
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
        route.endpoint = route.api + std::string(RESPONSES_SUFFIX);
        route.dialect  = ApiStandard::OPENAI_RESPONSES;
        route.auth     = AuthType::OPENAI_SUBSCRIPTION;
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
        route.endpoint = route.api + std::string(CHAT_SUFFIX);
        break;
    case ApiStandard::OPENAI_RESPONSES:
        route.endpoint = route.api + std::string(RESPONSES_SUFFIX);
        break;
    case ApiStandard::ANTHROPIC:
        route.endpoint = route.api + std::string(MESSAGES_SUFFIX);
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
