#include <doctest/doctest.h>

#include <unistd.h>

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <vector>

#include "providers/catalog.h"
#include "providers/pricing.h"
#include "providers/store.h"

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

struct IsolatedCatalog {
    std::filesystem::path dir;
    std::string old_xdg;
    bool had_xdg = false;

    IsolatedCatalog()
    {
        static int counter = 0;
        dir                = std::filesystem::temp_directory_path()
            / ("imza-pricing-test-" + std::to_string(::getpid()) + "-"
                + std::to_string(counter++));
        std::filesystem::create_directories(dir);
        if (const char* xdg = std::getenv("XDG_DATA_HOME")) {
            old_xdg = xdg;
            had_xdg = true;
        }
        setenv("XDG_DATA_HOME", dir.string().c_str(), 1);
        std::ignore
            = imza::save_catalog(dir / "imza" / "presets.json", test_catalog());
    }

    ~IsolatedCatalog()
    {
        if (had_xdg) {
            setenv("XDG_DATA_HOME", old_xdg.c_str(), 1);
        } else {
            unsetenv("XDG_DATA_HOME");
        }
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

} // namespace

TEST_CASE("pricing_table_from builds expected entries")
{
    struct Scenario {
        const char* name;
        const char* key;
        imza::ModelPricing expected;
    };
    const std::vector<Scenario> scenarios {
        { "bare OpenAI id", "gpt-4o-mini",
            { 0.00015, 0.00060, 0.000075, 0.0, 128000 } },
        { "provider-qualified OpenAI id", "openai/gpt-4o-mini",
            { 0.00015, 0.00060, 0.000075, 0.0, 128000 } },
        { "bare Anthropic id", "claude-sonnet-4",
            { 0.003, 0.015, 0.00030, 0.00375, 200000 } },
        { "provider-qualified Anthropic id", "anthropic/claude-sonnet-4",
            { 0.003, 0.015, 0.00030, 0.00375, 200000 } },
        { "zero-cost model", "glm-5-flash", { 0.0, 0.0, 0.0, 0.0, 128000 } },
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
    CHECK(table.count("costless-model") == 0);
}

TEST_CASE("pricing_for matches provider-qualified ids and is case-insensitive")
{
    IsolatedCatalog isolated;
    imza::ProviderStore store { imza::Config { } };

    const auto qualified = store.pricing_for("anthropic/claude-sonnet-4");
    const auto bare      = store.pricing_for("Claude-Sonnet-4");
    CHECK(qualified.input_per_1k == doctest::Approx(0.003));
    CHECK(bare.input_per_1k == doctest::Approx(0.003));
    CHECK(bare.cache_write_per_1k == doctest::Approx(0.00375));
}

TEST_CASE("pricing_for matches by substring")
{
    IsolatedCatalog isolated;
    imza::ProviderStore store { imza::Config { } };

    const auto p = store.pricing_for("openai/gpt-4o-mini-2024-07-18");
    CHECK(p.input_per_1k == doctest::Approx(0.00015));
}

TEST_CASE("pricing_for keeps zero-cost models and rejects unknown ones")
{
    IsolatedCatalog isolated;
    imza::ProviderStore store { imza::Config { } };

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
