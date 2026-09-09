#include <doctest/doctest.h>

#include <unistd.h>

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
                        "modalities": {"input": ["text"], "output": ["text"]},
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
}

TEST_CASE("catalog roundtrip through presets file")
{
    imza::Catalog catalog;
    catalog.fetched_at = 1756390000;
    imza::CachedProvider openrouter;
    openrouter.name                          = "OpenRouter";
    openrouter.api                           = "https://openrouter.ai/api/v1";
    openrouter.npm                           = "@ai-sdk/openai-compatible";
    openrouter.models["openai/gpt-5.5"].name = "GPT 5.5";
    catalog.providers["openrouter"]          = openrouter;

    const auto path = std::filesystem::temp_directory_path()
        / ("imza-catalog-test-" + std::to_string(::getpid()) + ".json");
    REQUIRE(imza::save_catalog(path, catalog) == imza::Status::OK);

    imza::Catalog loaded;
    CHECK(imza::load_catalog(path, loaded) == imza::Status::OK);
    REQUIRE(loaded.providers.count("openrouter") == 1);
    CHECK(loaded.providers.at("openrouter").name == "OpenRouter");
    CHECK(
        loaded.providers.at("openrouter").models.count("openai/gpt-5.5") == 1);
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

TEST_CASE("auth_from_npm maps ai-sdk packages")
{
    CHECK(
        imza::auth_from_npm("@ai-sdk/anthropic") == imza::AuthType::ANTHROPIC);
    CHECK(imza::auth_from_npm("@ai-sdk/anthropic/vertex")
        == imza::AuthType::ANTHROPIC);
    CHECK(imza::auth_from_npm("@ai-sdk/openai-compatible")
        == imza::AuthType::BEARER);
    CHECK(imza::auth_from_npm("@ai-sdk/openai") == imza::AuthType::BEARER);
    CHECK(imza::auth_from_npm("") == imza::AuthType::BEARER);
}

TEST_CASE("dialect_from_npm maps ai-sdk adapters")
{
    CHECK(imza::dialect_from_npm("@ai-sdk/openai")
        == imza::ApiStandard::OPENAI_RESPONSES);
    CHECK(imza::dialect_from_npm("@ai-sdk/openai-compatible")
        == imza::ApiStandard::OPENAI);
    CHECK(imza::dialect_from_npm("@ai-sdk/anthropic")
        == imza::ApiStandard::ANTHROPIC);
    CHECK(imza::dialect_from_npm("") == imza::ApiStandard::OPENAI);
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
    CHECK(catalog.providers.at("anthropic-subscription").name
        == "Anthropic Subscription");
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

TEST_CASE("resolve_route derives endpoints per dialect")
{
    imza::Catalog catalog;
    imza::CachedProvider openrouter;
    openrouter.name                 = "OpenRouter";
    openrouter.api                  = "https://openrouter.ai/api/v1";
    openrouter.npm                  = "@ai-sdk/openai-compatible";
    catalog.providers["openrouter"] = openrouter;

    imza::Connection conn;
    conn.id      = "openrouter";
    conn.api_key = "sk-or";

    const imza::Route openai
        = imza::resolve_route(conn, catalog, imza::ApiStandard::OPENAI);
    CHECK(openai.endpoint == "https://openrouter.ai/api/v1/chat/completions");
    CHECK(openai.api == "https://openrouter.ai/api/v1");
    CHECK(openai.auth == imza::AuthType::BEARER);
    CHECK(openai.api_key == "sk-or");

    const imza::Route anthropic
        = imza::resolve_route(conn, catalog, imza::ApiStandard::ANTHROPIC);
    CHECK(anthropic.endpoint == "https://openrouter.ai/api/v1/messages");

    const imza::Route responses = imza::resolve_route(
        conn, catalog, imza::ApiStandard::OPENAI_RESPONSES);
    CHECK(responses.endpoint == "https://openrouter.ai/api/v1/responses");
    CHECK(responses.dialect == imza::ApiStandard::OPENAI_RESPONSES);
}

TEST_CASE("resolve_route routes anthropic providers with x-api-key")
{
    imza::Catalog catalog;
    imza::CachedProvider anthropic_provider;
    anthropic_provider.name        = "Anthropic";
    anthropic_provider.api         = "https://api.anthropic.com/v1";
    anthropic_provider.npm         = "@ai-sdk/anthropic";
    catalog.providers["anthropic"] = anthropic_provider;

    imza::Connection conn;
    conn.id      = "anthropic";
    conn.api_key = "sk-ant";

    const imza::Route openai
        = imza::resolve_route(conn, catalog, imza::ApiStandard::OPENAI);
    CHECK(openai.endpoint == "https://api.anthropic.com/v1/chat/completions");
    CHECK(openai.api == "https://api.anthropic.com/v1");
    CHECK(openai.auth == imza::AuthType::ANTHROPIC);

    const imza::Route anthropic
        = imza::resolve_route(conn, catalog, imza::ApiStandard::ANTHROPIC);
    CHECK(anthropic.endpoint == "https://api.anthropic.com/v1/messages");
}

TEST_CASE("resolve_route uses stored endpoint for local and custom")
{
    imza::Catalog catalog;
    imza::Connection conn;
    conn.id       = "custom";
    conn.endpoint = "http://localhost:1234/v1/chat/completions";

    const imza::Route route
        = imza::resolve_route(conn, catalog, imza::ApiStandard::OPENAI);
    CHECK(route.endpoint == "http://localhost:1234/v1/chat/completions");
    CHECK(route.api == "http://localhost:1234/v1");
    CHECK(route.auth == imza::AuthType::NONE);

    conn.api_key = "secret";
    const imza::Route keyed
        = imza::resolve_route(conn, catalog, imza::ApiStandard::ANTHROPIC);
    CHECK(keyed.endpoint == "http://localhost:1234/v1/chat/completions");
    CHECK(keyed.dialect == imza::ApiStandard::OPENAI);
    CHECK(keyed.auth == imza::AuthType::BEARER);

    const imza::Route responses = imza::resolve_route(
        conn, catalog, imza::ApiStandard::OPENAI_RESPONSES);
    CHECK(responses.endpoint == "http://localhost:1234/v1/responses");
    CHECK(responses.dialect == imza::ApiStandard::OPENAI_RESPONSES);
}

TEST_CASE("resolve_route misses unknown providers")
{
    imza::Catalog catalog;
    imza::Connection conn;
    conn.id = "ghost";
    const imza::Route route
        = imza::resolve_route(conn, catalog, imza::ApiStandard::OPENAI);
    CHECK(route.endpoint.empty());
    CHECK(route.api.empty());
}

TEST_CASE("subscription routes use fixed endpoints and auth")
{
    imza::Catalog catalog;
    imza::Connection openai;
    openai.id         = "openai-subscription";
    openai.api_key    = "access";
    openai.account_id = "account";
    const imza::Route openai_route
        = imza::resolve_route(openai, catalog, imza::ApiStandard::ANTHROPIC);
    CHECK(openai_route.endpoint
        == "https://chatgpt.com/backend-api/codex/responses");
    CHECK(openai_route.dialect == imza::ApiStandard::OPENAI_RESPONSES);
    CHECK(openai_route.auth == imza::AuthType::OPENAI_SUBSCRIPTION);
    CHECK(openai_route.account_id == "account");

    imza::Connection anthropic;
    anthropic.id      = "anthropic-subscription";
    anthropic.api_key = "access";
    const imza::Route anthropic_route
        = imza::resolve_route(anthropic, catalog, imza::ApiStandard::OPENAI);
    CHECK(anthropic_route.endpoint == "https://api.anthropic.com/v1/messages");
    CHECK(anthropic_route.dialect == imza::ApiStandard::ANTHROPIC);
    CHECK(anthropic_route.auth == imza::AuthType::ANTHROPIC_SUBSCRIPTION);
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

    const auto anthropic_subscription
        = imza::auth_headers(AuthType::ANTHROPIC_SUBSCRIPTION, "oauth");
    REQUIRE(anthropic_subscription.size() == 3);
    CHECK(anthropic_subscription[0] == "Authorization: Bearer oauth");
    CHECK(anthropic_subscription[2] == "anthropic-beta: oauth-2025-04-20");

    const auto openai_subscription
        = imza::auth_headers(AuthType::OPENAI_SUBSCRIPTION, "oauth", "account");
    REQUIRE(openai_subscription.size() == 3);
    CHECK(openai_subscription[0] == "Authorization: Bearer oauth");
    CHECK(openai_subscription[1] == "originator: codex_cli_rs");
    CHECK(openai_subscription[2] == "ChatGPT-Account-Id: account");
}
