#pragma once

#include <json/json.h>

#include <string_view>

namespace ursa {

std::string write_json(const Json::Value& value);
Json::Value parse_json(std::string_view text);

} // namespace ursa
