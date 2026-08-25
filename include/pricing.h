#pragma once

#include <string_view>

#include "catalog.h"
#include "network.h"
#include "types.h"

namespace ursa {

void set_pricing_catalog(const Catalog& catalog);
ModelPricing get_pricing(std::string_view model);
double compute_cost(const Usage& usage, const ModelPricing& pricing);

} // namespace ursa
