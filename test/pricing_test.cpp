#include <doctest/doctest.h>

#include "catalog.h"
#include "pricing.h"
#include "types.h"

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

    ursa::CachedProvider openai;
    openai.name   = "OpenAI";
    openai.models = { { "gpt-4o-mini", mini } };

    ursa::CachedProvider anthropic;
    anthropic.name   = "Anthropic";
    anthropic.models = { { "claude-sonnet-4", sonnet } };

    ursa::CachedProvider zai;
    zai.name   = "Z.ai";
    zai.models = { { "glm-5-flash", free_model } };

    ursa::Catalog catalog;
    catalog.fetched_at = 1;
    catalog.providers  = { { "openai", openai },
          { "anthropic", anthropic },
          { "zai", zai } };
    return catalog;
}

} // namespace

TEST_CASE("get_pricing reads the catalog snapshot")
{
    ursa::set_pricing_catalog(test_catalog());

    const auto p = ursa::get_pricing("gpt-4o-mini");
    CHECK(p.input_per_1k == doctest::Approx(0.00015));
    CHECK(p.output_per_1k == doctest::Approx(0.00060));
    CHECK(p.cache_read_per_1k == doctest::Approx(0.000075));
    CHECK(p.cache_write_per_1k == doctest::Approx(0.0));
    CHECK(p.context_limit == 128000);
}

TEST_CASE("get_pricing matches provider-qualified ids and is case-insensitive")
{
    ursa::set_pricing_catalog(test_catalog());

    const auto qualified = ursa::get_pricing("anthropic/claude-sonnet-4");
    const auto bare      = ursa::get_pricing("Claude-Sonnet-4");
    CHECK(qualified.input_per_1k == doctest::Approx(0.003));
    CHECK(bare.input_per_1k == doctest::Approx(0.003));
    CHECK(bare.cache_write_per_1k == doctest::Approx(0.00375));
}

TEST_CASE("get_pricing matches by substring")
{
    ursa::set_pricing_catalog(test_catalog());

    const auto p = ursa::get_pricing("openai/gpt-4o-mini-2024-07-18");
    CHECK(p.input_per_1k == doctest::Approx(0.00015));
}

TEST_CASE("get_pricing keeps zero-cost models and rejects unknown ones")
{
    ursa::set_pricing_catalog(test_catalog());

    const auto free = ursa::get_pricing("zai/glm-5-flash");
    CHECK(free.input_per_1k == 0.0);
    CHECK(free.output_per_1k == 0.0);
    CHECK(free.context_limit == 128000);

    const auto unknown = ursa::get_pricing("some-unknown-model");
    CHECK(unknown.input_per_1k == 0.0);
    CHECK(unknown.output_per_1k == 0.0);
    CHECK(unknown.context_limit == 0);
}

TEST_CASE("get_pricing is empty before any catalog is set")
{
    ursa::set_pricing_catalog(ursa::Catalog { });
    const auto p = ursa::get_pricing("gpt-4o-mini");
    CHECK(p.context_limit == 0);
    CHECK(p.input_per_1k == 0.0);
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
    u.prompt      = 100000;
    u.completion  = 1000;
    u.cached_read = 60000;
    u.cached_write = 10000;
    u.total       = 101000;

    const double expected = 30.0 * 0.003 + 60.0 * 0.0003 + 10.0 * 0.00375
        + 1.0 * 0.015;
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
    u.prompt      = 1000;
    u.completion  = 0;
    u.cached_read = 800;
    u.cached_write = 900;
    u.total       = 1000;

    const double expected = 0.8 * 0.0001 + 0.2 * 0.001;
    CHECK(ursa::compute_cost(u, p) == doctest::Approx(expected));
}
