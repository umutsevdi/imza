#include <doctest/doctest.h>

#include <unistd.h>

#include <array>
#include <ctime>
#include <fstream>

#include "providers/catalog.h"

namespace {

std::string provider_json()
{
    return R"({
        "fetched_at": 0,
        "providers": {
            "openai": {
                "id": "openai",
                "name": "OpenAI",
                "api": "https://api.openai.com/v1",
                "npm": "@ai-sdk/openai",
                "env": ["OPENAI_API_KEY"],
                "doc": "https://platform.openai.com",
                "models": {
                    "gpt-5.5": {
                        "name": "GPT 5.5",
                        "description": "long text",
                        "attachment": true,
                        "modalities": {
                            "input": ["text", "image", "audio", "pdf"],
                            "output": ["text"]
                        },
                        "tool_call": true,
                        "reasoning": true,
                        "cost": {"input": 1.25, "output": 10.0,
                                 "cache_read": 0.125, "cache_write": null},
                        "limit": {"context": 272000, "output": 128000}
                    }
                }
            }
        }
    })";
}

} // namespace

TEST_CASE("load_catalog prunes models.dev fields")
{
    const auto path = std::filesystem::temp_directory_path()
        / ("imza-catalog-test-" + std::to_string(::getpid()) + ".json");
    {
        std::ofstream file(path);
        REQUIRE(file.good());
        file << provider_json();
    }

    imza::Catalog loaded;
    REQUIRE(imza::load_catalog(path, loaded) == imza::Status::OK);
    std::filesystem::remove(path);
    REQUIRE(loaded.providers.size() == 1);
    const auto& out = loaded.providers.at("openai");
    CHECK(out.name == "OpenAI");
    CHECK(out.api == "https://api.openai.com/v1");
    CHECK(out.npm == "@ai-sdk/openai");
    REQUIRE(out.models.size() == 1);
    const auto& model = out.models.at("gpt-5.5");
    CHECK(model.name == "GPT 5.5");
    REQUIRE(model.cost_input.has_value());
    CHECK(*model.cost_input == doctest::Approx(1.25));
    REQUIRE(model.cost_output.has_value());
    CHECK(*model.cost_output == doctest::Approx(10.0));
    REQUIRE(model.cost_cache_read.has_value());
    CHECK(*model.cost_cache_read == doctest::Approx(0.125));
    CHECK_FALSE(model.cost_cache_write.has_value());
    REQUIRE(model.context.has_value());
    CHECK(*model.context == 272000);
    REQUIRE(model.output.has_value());
    CHECK(*model.output == 128000);
    REQUIRE(model.tool_call.has_value());
    CHECK(*model.tool_call);
    REQUIRE(model.capabilities.has_value());
    CHECK(imza::has_capability(*model.capabilities, imza::Capabilities::IMAGE));
    CHECK(imza::has_capability(*model.capabilities, imza::Capabilities::PDF));
}

TEST_CASE("catalog roundtrip through presets file")
{
    imza::Catalog catalog;
    catalog.fetched_at = 1756390000;
    imza::CachedProvider openrouter;
    openrouter.name    = "OpenRouter";
    openrouter.api     = "https://openrouter.ai/api/v1";
    openrouter.npm     = "@ai-sdk/openai-compatible";
    auto& model        = openrouter.models["openai/gpt-5.5"];
    model.name         = "GPT 5.5";
    model.capabilities = imza::Capabilities::IMAGE | imza::Capabilities::PDF;
    catalog.providers["openrouter"] = openrouter;

    const auto path = std::filesystem::temp_directory_path()
        / ("imza-catalog-test-" + std::to_string(::getpid()) + ".json");
    REQUIRE(imza::save_catalog(path, catalog) == imza::Status::OK);

    imza::Catalog loaded;
    CHECK(imza::load_catalog(path, loaded) == imza::Status::OK);
    REQUIRE(loaded.providers.count("openrouter") == 1);
    CHECK(loaded.providers.at("openrouter").name == "OpenRouter");
    REQUIRE(
        loaded.providers.at("openrouter").models.count("openai/gpt-5.5") == 1);
    const auto& loaded_model
        = loaded.providers.at("openrouter").models.at("openai/gpt-5.5");
    REQUIRE(loaded_model.capabilities.has_value());
    CHECK(imza::has_capability(
        *loaded_model.capabilities, imza::Capabilities::IMAGE));
    CHECK(imza::has_capability(
        *loaded_model.capabilities, imza::Capabilities::PDF));
    CHECK(loaded.fetched_at == 1756390000);
    std::filesystem::remove(path);
}

TEST_CASE("load_catalog missing file yields empty catalog")
{
    imza::Catalog catalog;
    CHECK(imza::load_catalog("/nonexistent/imza/presets.json", catalog)
        == imza::Status::OK);
    CHECK(catalog.providers.empty());
    CHECK(catalog_stale(catalog));
}

TEST_CASE("catalog_stale respects the 7-day window")
{
    imza::Catalog catalog;
    CHECK(catalog_stale(catalog));
    catalog.fetched_at = static_cast<std::int64_t>(std::time(nullptr));
    CHECK_FALSE(catalog_stale(catalog));
    catalog.fetched_at -= 8 * 24 * 3600;
    CHECK(catalog_stale(catalog));
}

TEST_CASE("backfill_catalog_urls patches only matching empty provider URLs")
{
    imza::Catalog catalog;
    catalog.providers["anthropic"].npm       = "@ai-sdk/anthropic";
    catalog.providers["cerebras"].npm        = "@ai-sdk/cerebras";
    catalog.providers["groq"].npm            = "@ai-sdk/groq";
    catalog.providers["mistral"].npm         = "@ai-sdk/mistral";
    catalog.providers["openai"].npm          = "@ai-sdk/openai";
    catalog.providers["togetherai"].npm      = "@ai-sdk/togetherai";
    catalog.providers["xai"].npm             = "@ai-sdk/xai";
    catalog.providers["unknown"].npm         = "@ai-sdk/openai-compatible";
    catalog.providers["openai-explicit"].api = "https://example.com/v1";
    catalog.providers["openai-explicit"].npm = "@ai-sdk/openai";

    imza::backfill_catalog_urls(catalog);

    CHECK(catalog.providers.at("anthropic").api
        == "https://api.anthropic.com/v1");
    CHECK(catalog.providers.at("cerebras").api == "https://api.cerebras.ai/v1");
    CHECK(catalog.providers.at("groq").api == "https://api.groq.com/openai/v1");
    CHECK(catalog.providers.at("mistral").api == "https://api.mistral.ai/v1");
    CHECK(catalog.providers.at("openai").api == "https://api.openai.com/v1");
    CHECK(catalog.providers.at("togetherai").api
        == "https://api.together.xyz/v1");
    CHECK(catalog.providers.at("xai").api == "https://api.x.ai/v1");
    CHECK(catalog.providers.at("unknown").api.empty());
    CHECK(catalog.providers.at("openai-explicit").api
        == "https://example.com/v1");
    CHECK(catalog.providers.at("openai-subscription").name
        == "Open AI Subscription");
    CHECK_FALSE(catalog.providers.contains("anthropic-subscription"));
}

TEST_CASE("catalog_base uses only the catalog URL")
{
    imza::CachedProvider empty;
    empty.npm = "@ai-sdk/anthropic";
    CHECK(imza::catalog_base(empty).empty());

    imza::CachedProvider explicit_api;
    explicit_api.api = "https://openrouter.ai/api/v1/";
    explicit_api.npm = "@ai-sdk/openai-compatible";
    CHECK(imza::catalog_base(explicit_api) == "https://openrouter.ai/api/v1");
}

TEST_CASE("resolve_route covers connection and dialect scenarios")
{
    imza::Catalog catalog;
    imza::CachedProvider openrouter;
    openrouter.name                 = "OpenRouter";
    openrouter.api                  = "https://openrouter.ai/api/v1";
    openrouter.npm                  = "@ai-sdk/openai-compatible";
    catalog.providers["openrouter"] = openrouter;

    imza::CachedProvider anthropic;
    anthropic.name                 = "Anthropic";
    anthropic.api                  = "https://api.anthropic.com/v1";
    anthropic.npm                  = "@ai-sdk/anthropic";
    catalog.providers["anthropic"] = anthropic;

    imza::CachedProvider vertex;
    vertex.name                 = "Vertex";
    vertex.api                  = "https://api.anthropic.com/v1";
    vertex.npm                  = "@ai-sdk/anthropic/vertex";
    catalog.providers["vertex"] = vertex;

    struct RouteCase {
        const char* name;
        const char* connection_id;
        const char* connection_endpoint;
        const char* api_key;
        const char* account_id;
        imza::ApiStandard requested_dialect;
        const char* endpoint;
        const char* api;
        imza::ApiStandard dialect;
        imza::AuthType auth;
    };
    const std::array cases {
        RouteCase { "catalog OpenAI", "openrouter", "", "sk-or", "",
            imza::ApiStandard::OPENAI,
            "https://openrouter.ai/api/v1/chat/completions",
            "https://openrouter.ai/api/v1", imza::ApiStandard::OPENAI,
            imza::AuthType::BEARER },
        RouteCase { "catalog Anthropic dialect", "openrouter", "", "sk-or", "",
            imza::ApiStandard::ANTHROPIC,
            "https://openrouter.ai/api/v1/messages",
            "https://openrouter.ai/api/v1", imza::ApiStandard::ANTHROPIC,
            imza::AuthType::BEARER },
        RouteCase { "catalog responses dialect", "openrouter", "", "sk-or", "",
            imza::ApiStandard::OPENAI_RESPONSES,
            "https://openrouter.ai/api/v1/responses",
            "https://openrouter.ai/api/v1", imza::ApiStandard::OPENAI_RESPONSES,
            imza::AuthType::BEARER },
        RouteCase { "Anthropic provider OpenAI dialect", "anthropic", "",
            "sk-ant", "", imza::ApiStandard::OPENAI,
            "https://api.anthropic.com/v1/chat/completions",
            "https://api.anthropic.com/v1", imza::ApiStandard::OPENAI,
            imza::AuthType::ANTHROPIC },
        RouteCase { "Anthropic provider native dialect", "anthropic", "",
            "sk-ant", "", imza::ApiStandard::ANTHROPIC,
            "https://api.anthropic.com/v1/messages",
            "https://api.anthropic.com/v1", imza::ApiStandard::ANTHROPIC,
            imza::AuthType::ANTHROPIC },
        RouteCase { "vertex npm subpath keeps Anthropic auth", "vertex", "",
            "sk-ant", "", imza::ApiStandard::ANTHROPIC,
            "https://api.anthropic.com/v1/messages",
            "https://api.anthropic.com/v1", imza::ApiStandard::ANTHROPIC,
            imza::AuthType::ANTHROPIC },
        RouteCase { "custom endpoint without key", "custom",
            "http://localhost:1234/v1/chat/completions", "", "",
            imza::ApiStandard::OPENAI,
            "http://localhost:1234/v1/chat/completions",
            "http://localhost:1234/v1", imza::ApiStandard::OPENAI,
            imza::AuthType::NONE },
        RouteCase { "custom endpoint with key", "custom",
            "http://localhost:1234/v1/chat/completions", "secret", "",
            imza::ApiStandard::ANTHROPIC,
            "http://localhost:1234/v1/chat/completions",
            "http://localhost:1234/v1", imza::ApiStandard::OPENAI,
            imza::AuthType::BEARER },
        RouteCase { "custom responses endpoint", "custom",
            "http://localhost:1234/v1/chat/completions", "secret", "",
            imza::ApiStandard::OPENAI_RESPONSES,
            "http://localhost:1234/v1/responses", "http://localhost:1234/v1",
            imza::ApiStandard::OPENAI_RESPONSES, imza::AuthType::BEARER },
        RouteCase { "unknown provider", "ghost", "", "", "",
            imza::ApiStandard::OPENAI, "", "", imza::ApiStandard::OPENAI,
            imza::AuthType::BEARER },
        RouteCase { "OpenAI subscription", "openai-subscription", "", "access",
            "account", imza::ApiStandard::ANTHROPIC,
            "https://chatgpt.com/backend-api/codex/responses",
            "https://chatgpt.com/backend-api/codex",
            imza::ApiStandard::OPENAI_RESPONSES,
            imza::AuthType::OPENAI_SUBSCRIPTION },
    };

    for (const auto& route_case : cases) {
        CAPTURE(route_case.name);
        imza::Connection connection;
        connection.id         = route_case.connection_id;
        connection.endpoint   = route_case.connection_endpoint;
        connection.api_key    = route_case.api_key;
        connection.account_id = route_case.account_id;

        const imza::Route route = imza::resolve_route(
            connection, catalog, route_case.requested_dialect);
        CHECK(route.endpoint == route_case.endpoint);
        CHECK(route.api == route_case.api);
        CHECK(route.dialect == route_case.dialect);
        CHECK(route.auth == route_case.auth);
        CHECK(route.api_key == route_case.api_key);
        CHECK(route.account_id == route_case.account_id);
    }

    CHECK(imza::auth_from_npm("") == imza::AuthType::BEARER);
    CHECK(imza::dialect_from_npm("") == imza::ApiStandard::OPENAI);
}

TEST_CASE("auth_headers by auth type")
{
    using imza::AuthType;
    CHECK(imza::auth_headers(AuthType::NONE, "k").empty());
    CHECK(imza::auth_headers(AuthType::BEARER, "").empty());

    const auto bearer = imza::auth_headers(AuthType::BEARER, "k");
    REQUIRE(bearer.size() == 1);
    CHECK(bearer[0] == "Authorization: Bearer k");

    const auto anthropic = imza::auth_headers(AuthType::ANTHROPIC, "k");
    REQUIRE(anthropic.size() == 2);
    CHECK(anthropic[0] == "x-api-key: k");
    CHECK(anthropic[1] == "anthropic-version: 2023-06-01");

    const auto openai_subscription
        = imza::auth_headers(AuthType::OPENAI_SUBSCRIPTION, "oauth", "account");
    REQUIRE(openai_subscription.size() == 3);
    CHECK(openai_subscription[0] == "Authorization: Bearer oauth");
    CHECK(openai_subscription[1] == "originator: codex_cli_rs");
    CHECK(openai_subscription[2] == "ChatGPT-Account-Id: account");
}

TEST_CASE("resolve_route stamps the client User-Agent")
{
    imza::Catalog catalog;
    const auto user_agent_for = [&](std::string_view id) {
        imza::Connection connection;
        connection.id      = std::string(id);
        connection.api_key = "key";
        return imza::resolve_route(
            connection, catalog, imza::ApiStandard::OPENAI)
            .user_agent;
    };

    CHECK(user_agent_for("zai") == "User-Agent: Pi/3.1.0");
    CHECK(user_agent_for("zai-coding-plan") == "User-Agent: Pi/3.1.0");
    CHECK(user_agent_for("zhipuai") == "User-Agent: Pi/3.1.0");
    CHECK(user_agent_for("zhipuai-coding-plan") == "User-Agent: Pi/3.1.0");
    CHECK(user_agent_for("kimi-for-coding") == "User-Agent: hermes-agent/1.0");

    CHECK(user_agent_for("moonshotai").starts_with("User-Agent: imza/"));
    CHECK(user_agent_for("openrouter").starts_with("User-Agent: imza/"));
    CHECK(user_agent_for("ghost").starts_with("User-Agent: imza/"));

    CHECK(user_agent_for("openai-subscription").empty());
    CHECK(user_agent_for("openai-subscription/plus").empty());

    imza::Connection custom;
    custom.id       = "custom";
    custom.endpoint = "http://localhost:1234/v1";
    custom.api_key  = "key";
    CHECK(imza::resolve_route(custom, catalog, imza::ApiStandard::OPENAI)
            .user_agent.starts_with("User-Agent: imza/"));
}

TEST_CASE("resolve_route stamps the OpenCode session id")
{
    imza::Catalog catalog;
    const auto session_for = [&](std::string_view id) {
        imza::Connection connection;
        connection.id      = std::string(id);
        connection.api_key = "key";
        return imza::resolve_route(
            connection, catalog, imza::ApiStandard::OPENAI, "abc-123")
            .opencode_session;
    };

    CHECK(session_for("opencode") == "abc-123");
    CHECK(session_for("opencode-go") == "abc-123");

    CHECK(session_for("zai").empty());
    CHECK(session_for("moonshotai").empty());
    CHECK(session_for("openai-subscription").empty());

    imza::Connection custom;
    custom.id       = "custom";
    custom.endpoint = "http://localhost:1234/v1";
    custom.api_key  = "key";
    CHECK(imza::resolve_route(
        custom, catalog, imza::ApiStandard::OPENAI, "abc-123")
            .opencode_session.empty());
}
