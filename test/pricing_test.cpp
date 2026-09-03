#include <doctest/doctest.h>

#include <unistd.h>

#include <cstdlib>
#include <ctime>
#include <filesystem>

#include "core/catalog.h"
#include "core/pricing.h"
#include "subsystems/provider_store.h"

namespace {

ursa::Catalog test_catalog()
{
    ursa::CachedModel mini;
    mini.cost_input      = 0.15;
    mini.cost_output     = 0.60;
    mini.cost_cache_read = 0.075;
    mini.context         = 128000;

    ursa::CachedModel sonnet;
    sonnet.cost_input       = 3.0;
    sonnet.cost_output      = 15.0;
    sonnet.cost_cache_read  = 0.30;
    sonnet.cost_cache_write = 3.75;
    sonnet.context          = 200000;

    ursa::CachedModel free_model;
    free_model.cost_input  = 0.0;
    free_model.cost_output = 0.0;
    free_model.context     = 128000;

    ursa::CachedModel costless;
    costless.context = 4096;

    ursa::CachedProvider openai;
    openai.name   = "OpenAI";
    openai.models = { { "gpt-4o-mini", mini }, { "costless-model", costless } };

    ursa::CachedProvider anthropic;
    anthropic.name   = "Anthropic";
    anthropic.models = { { "claude-sonnet-4", sonnet } };

    ursa::CachedProvider zai;
    zai.name   = "Z.ai";
    zai.models = { { "glm-5-flash", free_model } };

    ursa::Catalog catalog;
    catalog.fetched_at = 1;
    catalog.providers
        = { { "openai", openai }, { "anthropic", anthropic }, { "zai", zai } };
    return catalog;
}

struct IsolatedCatalog {
    std::filesystem::path dir;
    std::string old_xdg;
    bool had_xdg = false;

    IsolatedCatalog()
    {
        static int counter = 0;
        dir                = std::filesystem::temp_directory_path()
            / ("ursa-pricing-test-" + std::to_string(::getpid()) + "-"
                + std::to_string(counter++));
        std::filesystem::create_directories(dir);
        if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
            old_xdg = xdg;
            had_xdg = true;
        }
        setenv("XDG_CONFIG_HOME", dir.string().c_str(), 1);
        std::ignore
            = ursa::save_catalog(dir / "ursa" / "presets.json", test_catalog());
    }

    ~IsolatedCatalog()
    {
        if (had_xdg) {
            setenv("XDG_CONFIG_HOME", old_xdg.c_str(), 1);
        } else {
            unsetenv("XDG_CONFIG_HOME");
        }
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

} // namespace

TEST_CASE("pricing_table_from builds bare and provider-qualified keys")
{
    const auto table = ursa::pricing_table_from(test_catalog());

    REQUIRE(table.count("gpt-4o-mini") == 1);
    REQUIRE(table.count("openai/gpt-4o-mini") == 1);
    const auto& mini = table.at("openai/gpt-4o-mini");
    CHECK(mini.input_per_1k == doctest::Approx(0.00015));
    CHECK(mini.output_per_1k == doctest::Approx(0.00060));
    CHECK(mini.cache_read_per_1k == doctest::Approx(0.000075));
    CHECK(mini.cache_write_per_1k == doctest::Approx(0.0));
    CHECK(mini.context_limit == 128000);

    REQUIRE(table.count("claude-sonnet-4") == 1);
    REQUIRE(table.count("anthropic/claude-sonnet-4") == 1);
    CHECK(table.at("anthropic/claude-sonnet-4").input_per_1k
        == doctest::Approx(0.003));
    CHECK(table.at("anthropic/claude-sonnet-4").cache_write_per_1k
        == doctest::Approx(0.00375));
}

TEST_CASE("pricing_table_from keeps zero-cost models and drops costless ones")
{
    const auto table = ursa::pricing_table_from(test_catalog());

    REQUIRE(table.count("glm-5-flash") == 1);
    CHECK(table.at("glm-5-flash").input_per_1k == 0.0);
    CHECK(table.at("glm-5-flash").context_limit == 128000);

    CHECK(table.count("costless-model") == 0);
}

TEST_CASE("pricing_for reads the store's catalog snapshot")
{
    IsolatedCatalog isolated;
    ursa::ProviderStore store { ursa::Config { } };

    const auto p = store.pricing_for("gpt-4o-mini");
    CHECK(p.input_per_1k == doctest::Approx(0.00015));
    CHECK(p.context_limit == 128000);
}

TEST_CASE("pricing_for matches provider-qualified ids and is case-insensitive")
{
    IsolatedCatalog isolated;
    ursa::ProviderStore store { ursa::Config { } };

    const auto qualified = store.pricing_for("anthropic/claude-sonnet-4");
    const auto bare      = store.pricing_for("Claude-Sonnet-4");
    CHECK(qualified.input_per_1k == doctest::Approx(0.003));
    CHECK(bare.input_per_1k == doctest::Approx(0.003));
    CHECK(bare.cache_write_per_1k == doctest::Approx(0.00375));
}

TEST_CASE("pricing_for matches by substring")
{
    IsolatedCatalog isolated;
    ursa::ProviderStore store { ursa::Config { } };

    const auto p = store.pricing_for("openai/gpt-4o-mini-2024-07-18");
    CHECK(p.input_per_1k == doctest::Approx(0.00015));
}

TEST_CASE("pricing_for keeps zero-cost models and rejects unknown ones")
{
    IsolatedCatalog isolated;
    ursa::ProviderStore store { ursa::Config { } };

    const auto free = store.pricing_for("zai/glm-5-flash");
    CHECK(free.input_per_1k == 0.0);
    CHECK(free.output_per_1k == 0.0);
    CHECK(free.context_limit == 128000);

    const auto unknown = store.pricing_for("some-unknown-model");
    CHECK(unknown.input_per_1k == 0.0);
    CHECK(unknown.output_per_1k == 0.0);
    CHECK(unknown.context_limit == 0);

    const auto empty = store.pricing_for("");
    CHECK(empty.context_limit == 0);
}

TEST_CASE("compute_cost scales by token counts")
{
    ursa::ModelPricing p { 0.001, 0.002, 0.0, 0.0, 1000 };

    ursa::Usage u { .prompt = 1000, .completion = 500, .total = 1500 };
    CHECK(ursa::compute_cost(u, p) == doctest::Approx(0.001 + 0.001));
}

TEST_CASE("compute_cost bills cached tokens at cache rates when present")
{
    ursa::ModelPricing p;
    p.input_per_1k       = 0.003;
    p.output_per_1k      = 0.015;
    p.cache_read_per_1k  = 0.0003;
    p.cache_write_per_1k = 0.00375;

    ursa::Usage u;
    u.prompt       = 100000;
    u.completion   = 1000;
    u.cached_read  = 60000;
    u.cached_write = 10000;
    u.total        = 101000;

    const double expected
        = 30.0 * 0.003 + 60.0 * 0.0003 + 10.0 * 0.00375 + 1.0 * 0.015;
    CHECK(ursa::compute_cost(u, p) == doctest::Approx(expected));
}

TEST_CASE("compute_cost falls back to input rate when cache rates are absent")
{
    ursa::ModelPricing p;
    p.input_per_1k  = 0.001;
    p.output_per_1k = 0.002;

    ursa::Usage u;
    u.prompt      = 2000;
    u.completion  = 0;
    u.cached_read = 1000;
    u.total       = 2000;

    CHECK(ursa::compute_cost(u, p) == doctest::Approx(0.002));
}

TEST_CASE("compute_cost is safe when cached tokens exceed prompt")
{
    ursa::ModelPricing p;
    p.input_per_1k      = 0.001;
    p.output_per_1k     = 0.002;
    p.cache_read_per_1k = 0.0001;

    ursa::Usage u;
    u.prompt       = 1000;
    u.completion   = 0;
    u.cached_read  = 800;
    u.cached_write = 900;
    u.total        = 1000;

    const double expected = 0.8 * 0.0001 + 0.2 * 0.001;
    CHECK(ursa::compute_cost(u, p) == doctest::Approx(expected));
}
