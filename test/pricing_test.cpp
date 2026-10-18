#include <doctest/doctest.h>

#include <unistd.h>

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <vector>

#include "providers/catalog.h"
#include "providers/pricing.h"
#include "providers/store.h"
#include "test_fs.h"
#include "test_state.h"

namespace {

imza::Catalog test_catalog()
{
    imza::CachedModel mini;
    mini.cost_input      = 0.15;
    mini.cost_output     = 0.60;
    mini.cost_cache_read = 0.075;
    mini.context         = 128000;

    imza::CachedModel sonnet;
    sonnet.cost_input       = 3.0;
    sonnet.cost_output      = 15.0;
    sonnet.cost_cache_read  = 0.30;
    sonnet.cost_cache_write = 3.75;
    sonnet.context          = 200000;

    imza::CachedModel free_model;
    free_model.cost_input  = 0.0;
    free_model.cost_output = 0.0;
    free_model.context     = 128000;

    imza::CachedModel costless;
    costless.context = 4096;

    imza::CachedProvider openai;
    openai.name   = "OpenAI";
    openai.models = { { "gpt-4o-mini", mini }, { "costless-model", costless } };

    imza::CachedProvider anthropic;
    anthropic.name   = "Anthropic";
    anthropic.models = { { "claude-sonnet-4", sonnet } };

    imza::CachedProvider zai;
    zai.name   = "Z.ai";
    zai.models = { { "glm-5-flash", free_model } };

    imza::Catalog catalog;
    catalog.fetched_at = 1;
    catalog.providers
        = { { "openai", openai }, { "anthropic", anthropic }, { "zai", zai } };
    return catalog;
}

// Data home with a fetched catalog carrying the pricing rows above.
struct CatalogHome : imza::test::IsolatedDataHome {
    CatalogHome()
    {
        std::ignore
            = imza::save_catalog(imza_dir() / "presets.json", test_catalog());
    }
};

} // namespace

TEST_CASE("pricing_table_from keys only by provider-qualified id")
{
    struct Scenario {
        const char* name;
        const char* key;
        imza::ModelPricing expected;
    };
    const std::vector<Scenario> scenarios {
        { "OpenAI id", "openai/gpt-4o-mini",
            { 0.00015, 0.00060, 0.000075, 0.0, 128000 } },
        { "Anthropic id", "anthropic/claude-sonnet-4",
            { 0.003, 0.015, 0.00030, 0.00375, 200000 } },
        { "zero-cost model", "zai/glm-5-flash",
            { 0.0, 0.0, 0.0, 0.0, 128000 } },
    };
    const auto table = imza::pricing_table_from(test_catalog());

    for (const auto& scenario : scenarios) {
        CAPTURE(scenario.name);
        REQUIRE(table.count(scenario.key) == 1);
        const auto& actual = table.at(scenario.key);
        CHECK(actual.input_per_1k
            == doctest::Approx(scenario.expected.input_per_1k));
        CHECK(actual.output_per_1k
            == doctest::Approx(scenario.expected.output_per_1k));
        CHECK(actual.cache_read_per_1k
            == doctest::Approx(scenario.expected.cache_read_per_1k));
        CHECK(actual.cache_write_per_1k
            == doctest::Approx(scenario.expected.cache_write_per_1k));
        CHECK(actual.context_limit == scenario.expected.context_limit);
    }
    // A bare id is ambiguous, so it is never indexed.
    CHECK(table.count("gpt-4o-mini") == 0);
    CHECK(table.count("claude-sonnet-4") == 0);
    CHECK(table.count("costless-model") == 0);
    CHECK(table.count("openai/costless-model") == 0);
}

TEST_CASE(
    "pricing_for is connection-scoped, case-insensitive, and never borrows")
{
    CatalogHome home;
    imza::ProviderStore store { imza::test::make_config_store() };

    // Case-insensitive within the owning connection.
    const auto sonnet = store.pricing_for("Claude-Sonnet-4", "anthropic");
    CHECK(sonnet.input_per_1k == doctest::Approx(0.003));
    CHECK(sonnet.cache_write_per_1k == doctest::Approx(0.00375));

    // A decorated id resolves to its base row.
    const auto decorated
        = store.pricing_for("gpt-4o-mini-2024-07-18", "openai");
    CHECK(decorated.input_per_1k == doctest::Approx(0.00015));

    // The model exists under openai, not anthropic: a lookup scoped to
    // anthropic must not resolve to openai's price.
    const auto borrowed = store.pricing_for("gpt-4o-mini", "anthropic");
    CHECK(borrowed.input_per_1k == 0.0);
    CHECK(borrowed.context_limit == 0);

    const auto own = store.pricing_for("gpt-4o-mini", "openai");
    CHECK(own.input_per_1k == doctest::Approx(0.00015));
}

TEST_CASE("pricing_for keeps zero-cost models and rejects unknown ones")
{
    CatalogHome home;
    imza::ProviderStore store { imza::test::make_config_store() };

    const auto free = store.pricing_for("glm-5-flash", "zai");
    CHECK(free.input_per_1k == 0.0);
    CHECK(free.output_per_1k == 0.0);
    CHECK(free.context_limit == 128000);

    const auto unknown = store.pricing_for("some-unknown-model", "zai");
    CHECK(unknown.input_per_1k == 0.0);
    CHECK(unknown.output_per_1k == 0.0);
    CHECK(unknown.context_limit == 0);

    const auto empty = store.pricing_for("", "zai");
    CHECK(empty.context_limit == 0);
}

TEST_CASE("compute_cost handles cache-rate scenarios")
{
    struct Scenario {
        const char* name;
        imza::ModelPricing pricing;
        imza::Usage usage;
        double expected;
    };
    const std::vector<Scenario> scenarios {
        { "explicit cache read and write rates",
            { 0.003, 0.015, 0.0003, 0.00375, 0 },
            { 100000, 1000, 60000, 10000, 101000 },
            30.0 * 0.003 + 60.0 * 0.0003 + 10.0 * 0.00375 + 1.0 * 0.015 },
        { "absent cache rates fall back to input rate",
            { 0.001, 0.002, 0.0, 0.0, 0 }, { 2000, 0, 1000, 0, 2000 }, 0.002 },
        { "cached tokens exceed prompt", { 0.001, 0.002, 0.0001, 0.0, 0 },
            { 1000, 0, 800, 900, 1000 }, 0.8 * 0.0001 + 0.2 * 0.001 },
    };

    for (const auto& scenario : scenarios) {
        CAPTURE(scenario.name);
        CHECK(imza::compute_cost(scenario.usage, scenario.pricing)
            == doctest::Approx(scenario.expected));
    }
}
