#include <doctest/doctest.h>
#include <json/json.h>

#include <unistd.h>

#include <ctime>

#include "catalog.h"

namespace {

Json::Value parse_value(const std::string& text)
{
    return ursa::parse_json(text);
}

TEST_CASE("trim_provider prunes models.dev fields")
{
    const auto src = parse_value(R"({
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
    })");

    ursa::CachedProvider out;
    REQUIRE(ursa::trim_provider(src, out) == ursa::Status::OK);
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
    ursa::Catalog catalog;
    catalog.fetched_at = 1756390000;
    const auto src = parse_value(R"({
        "name": "OpenRouter",
        "api": "https://openrouter.ai/api/v1",
        "npm": "@ai-sdk/openai-compatible",
        "models": {"openai/gpt-5.5": {"name": "GPT 5.5"}}
    })");
    REQUIRE(ursa::trim_provider(src, catalog.providers["openrouter"])
        == ursa::Status::OK);

    const auto path = std::filesystem::temp_directory_path()
        / ("ursa-catalog-test-" + std::to_string(::getpid()) + ".json");
    REQUIRE(ursa::save_catalog(path, catalog) == ursa::Status::OK);

    ursa::Catalog loaded;
    CHECK(ursa::load_catalog(path, loaded) == ursa::Status::OK);
    REQUIRE(loaded.providers.count("openrouter") == 1);
    CHECK(loaded.providers.at("openrouter").name == "OpenRouter");
    CHECK(loaded.providers.at("openrouter").models.count("openai/gpt-5.5")
        == 1);
    CHECK(loaded.fetched_at == 1756390000);
    std::filesystem::remove(path);
}

TEST_CASE("load_catalog missing file yields empty catalog")
{
    ursa::Catalog catalog;
    CHECK(ursa::load_catalog("/nonexistent/ursa/presets.json", catalog)
        == ursa::Status::OK);
    CHECK(catalog.providers.empty());
    CHECK(catalog_stale(catalog));
}

TEST_CASE("catalog_stale respects the 7-day window")
{
    ursa::Catalog catalog;
    CHECK(catalog_stale(catalog));
    catalog.fetched_at = static_cast<std::int64_t>(std::time(nullptr));
    CHECK_FALSE(catalog_stale(catalog));
    catalog.fetched_at
        -= 8 * 24 * 3600;
    CHECK(catalog_stale(catalog));
}

TEST_CASE("whitelist gates providers")
{
    CHECK(ursa::whitelisted_provider("openai"));
    CHECK(ursa::whitelisted_provider("zai-coding-plan"));
    CHECK_FALSE(ursa::whitelisted_provider("some-random-provider"));
}

TEST_CASE("auth_from_npm maps ai-sdk packages")
{
    CHECK(ursa::auth_from_npm("@ai-sdk/anthropic")
        == ursa::AuthType::ANTHROPIC);
    CHECK(ursa::auth_from_npm("@ai-sdk/anthropic/vertex")
        == ursa::AuthType::ANTHROPIC);
    CHECK(ursa::auth_from_npm("@ai-sdk/openai-compatible")
        == ursa::AuthType::BEARER);
    CHECK(ursa::auth_from_npm("@ai-sdk/openai") == ursa::AuthType::BEARER);
    CHECK(ursa::auth_from_npm("") == ursa::AuthType::BEARER);
}

TEST_CASE("catalog_base falls back for SDK-default providers")
{
    ursa::CachedProvider anthropic;
    anthropic.npm = "@ai-sdk/anthropic";
    CHECK(ursa::catalog_base(anthropic) == "https://api.anthropic.com/v1");

    ursa::CachedProvider openai;
    openai.npm = "@ai-sdk/openai";
    CHECK(ursa::catalog_base(openai) == "https://api.openai.com/v1");

    ursa::CachedProvider explicit_api;
    explicit_api.api = "https://openrouter.ai/api/v1/";
    explicit_api.npm = "@ai-sdk/openai-compatible";
    CHECK(ursa::catalog_base(explicit_api) == "https://openrouter.ai/api/v1");
}

TEST_CASE("resolve_route derives endpoints per dialect")
{
    ursa::Catalog catalog;
    const auto src = parse_value(R"({
        "name": "OpenRouter",
        "api": "https://openrouter.ai/api/v1",
        "npm": "@ai-sdk/openai-compatible"
    })");
    REQUIRE(ursa::trim_provider(src, catalog.providers["openrouter"])
        == ursa::Status::OK);

    ursa::Connection conn;
    conn.id          = "openrouter";
    conn.provider_id = "openrouter";
    conn.api_key     = "sk-or";

    const ursa::Route openai
        = ursa::resolve_route(conn, catalog, ursa::ApiStandard::OPENAI);
    CHECK(openai.endpoint
        == "https://openrouter.ai/api/v1/chat/completions");
    CHECK(openai.api == "https://openrouter.ai/api/v1");
    CHECK(openai.auth == ursa::AuthType::BEARER);
    CHECK(openai.api_key == "sk-or");

    const ursa::Route anthropic
        = ursa::resolve_route(conn, catalog, ursa::ApiStandard::ANTHROPIC);
    CHECK(anthropic.endpoint == "https://openrouter.ai/api/v1/messages");
}

TEST_CASE("resolve_route routes anthropic providers with x-api-key")
{
    ursa::Catalog catalog;
    const auto src = parse_value(R"({
        "name": "Anthropic",
        "npm": "@ai-sdk/anthropic"
    })");
    REQUIRE(ursa::trim_provider(src, catalog.providers["anthropic"])
        == ursa::Status::OK);

    ursa::Connection conn;
    conn.id          = "anthropic";
    conn.provider_id = "anthropic";
    conn.api_key     = "sk-ant";

    const ursa::Route openai
        = ursa::resolve_route(conn, catalog, ursa::ApiStandard::OPENAI);
    CHECK(openai.endpoint
        == "https://api.anthropic.com/v1/chat/completions");
    CHECK(openai.api == "https://api.anthropic.com/v1");
    CHECK(openai.auth == ursa::AuthType::ANTHROPIC);

    const ursa::Route anthropic
        = ursa::resolve_route(conn, catalog, ursa::ApiStandard::ANTHROPIC);
    CHECK(anthropic.endpoint == "https://api.anthropic.com/v1/messages");
}

TEST_CASE("resolve_route uses stored endpoint for local and custom")
{
    ursa::Catalog catalog;
    ursa::Connection conn;
    conn.id          = "custom";
    conn.provider_id = "custom";
    conn.endpoint    = "http://localhost:1234/v1/chat/completions";

    const ursa::Route route
        = ursa::resolve_route(conn, catalog, ursa::ApiStandard::OPENAI);
    CHECK(route.endpoint == "http://localhost:1234/v1/chat/completions");
    CHECK(route.api == "http://localhost:1234/v1");
    CHECK(route.auth == ursa::AuthType::NONE);

    conn.api_key = "secret";
    const ursa::Route keyed
        = ursa::resolve_route(conn, catalog, ursa::ApiStandard::ANTHROPIC);
    CHECK(keyed.endpoint == "http://localhost:1234/v1/chat/completions");
    CHECK(keyed.dialect == ursa::ApiStandard::OPENAI);
    CHECK(keyed.auth == ursa::AuthType::BEARER);
}

TEST_CASE("resolve_route misses unknown providers")
{
    ursa::Catalog catalog;
    ursa::Connection conn;
    conn.id          = "ghost";
    conn.provider_id = "ghost";
    const ursa::Route route
        = ursa::resolve_route(conn, catalog, ursa::ApiStandard::OPENAI);
    CHECK(route.endpoint.empty());
    CHECK(route.api.empty());
}

TEST_CASE("auth_headers by auth type")
{
    using ursa::AuthType;
    CHECK(ursa::auth_headers(AuthType::NONE, "k").empty());
    CHECK(ursa::auth_headers(AuthType::BEARER, "").empty());

    const auto bearer = ursa::auth_headers(AuthType::BEARER, "k");
    REQUIRE(bearer.size() == 1);
    CHECK(bearer[0] == "Authorization: Bearer k");

    const auto anthropic = ursa::auth_headers(AuthType::ANTHROPIC, "k");
    REQUIRE(anthropic.size() == 2);
    CHECK(anthropic[0] == "x-api-key: k");
    CHECK(anthropic[1] == "anthropic-version: 2023-06-01");
}

} // namespace
