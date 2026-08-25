#pragma once

#include "network.h"
#include "types.h"

namespace ursa {

ModelPricing get_pricing(const Config& cfg);
double compute_cost(const Usage& usage, const ModelPricing& pricing);

} // namespace ursa
