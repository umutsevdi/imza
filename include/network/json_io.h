#pragma once

#include <string>
#include <string_view>

#include "common/types.h"
#include "network/json.h"

namespace imza {

// Parse arbitrary JSON. Returns a null-typed value on malformed input.
JsonValue parse_json(std::string_view text);
std::string media_data_url(const Attachment& media);

} // namespace imza