#pragma once

#include <json/json.h>

#include <string>
#include <string_view>

#include "common/types.h"

namespace imza {

std::string write_json(const Json::Value& value);
std::string write_pretty_json(const Json::Value& value);
Json::Value parse_json(std::string_view text);
std::string media_data_url(const Attachment& media);

} // namespace imza
