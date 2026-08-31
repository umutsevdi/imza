#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "network/network.h"

namespace ursa {

struct ModelInfo {
    std::string id;
    std::string name;
    std::optional<std::uint64_t> context_length;
};

Status parse_models_response(
    std::string_view body, std::vector<ModelInfo>& out);
Status fetch_models(const Route& route, std::vector<ModelInfo>& out);

} // namespace ursa
