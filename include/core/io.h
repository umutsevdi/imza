#pragma once

#include <filesystem>
#include <string_view>

#include <json/json.h>

#include "common/types.h"

namespace ursa {

// Serializes root to path via a .tmp sibling + rename. Creates the parent
// directory. Empty indentation produces compact output.
Status write_json_file(const std::filesystem::path& path,
    const Json::Value& root, std::string_view indentation);

} // namespace ursa
