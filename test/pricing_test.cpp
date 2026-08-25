#include <doctest/doctest.h>

#include "pricing.h"
#include "types.h"

namespace {

TEST_CASE("get_pricing prefers config override over builtin table")
{
    ursa::Config cfg;
    cfg.model = "gpt-4o";
    cfg.pricing = ursa::ModelPricing { 0.001, 0.002, 99999 };
    const auto p = ursa::get_pricing(cfg);
    CHECK(p.input_per_1k == 0.001);
    CHECK(p.context_limit == 99999);
}

TEST_CASE("get_pricing matches builtin by substring")
{
    ursa::Config cfg;
    cfg.model = "openai/gpt-4o-mini";
    const auto p = ursa::get_pricing(cfg);
    CHECK(p.context_limit == 128000);
    CHECK(p.input_per_1k == 0.00015);
}

TEST_CASE("get_pricing returns empty for unknown model")
{
    ursa::Config cfg;
    cfg.model = "some-unknown-model";
    const auto p = ursa::get_pricing(cfg);
    CHECK(p.context_limit == 0);
    CHECK(p.input_per_1k == 0.0);
}

TEST_CASE("compute_cost scales by token counts")
{
    ursa::ModelPricing p { 0.001, 0.002, 1000 };
    ursa::Usage u { 1000, 500, 1500 };
    CHECK(ursa::compute_cost(u, p) == doctest::Approx(0.001 + 0.001));
}

} // namespace
