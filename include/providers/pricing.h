#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "providers/catalog.h"

namespace imza {

struct ModelPricing {
    double input_per_1k         = 0.0;
    double output_per_1k        = 0.0;
    double cache_read_per_1k    = 0.0;
    double cache_write_per_1k   = 0.0;
    std::uint64_t context_limit = 0;
};

std::map<std::string, ModelPricing> pricing_table_from(const Catalog& catalog);
double compute_cost(const Usage& usage, const ModelPricing& pricing);

} // namespace imza
