#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include <json/json.h>

#include "common/types.h"

namespace imza {

// Serializes root to path via a .tmp sibling + rename. Creates the parent
// directory. Empty indentation produces compact output.
Status write_json_file(const std::filesystem::path& path,
    const Json::Value& root, std::string_view indentation);

// Reads a file completely.
std::optional<std::string> read_text_file(const std::filesystem::path& path);

// nullopt when absent, unreadable, or malformed; check existence first when
// absent must differ from malformed.
std::optional<Json::Value> read_json_file(const std::filesystem::path& path);

// Sidecar-locked read-modify-write: absent or malformed input becomes an
// empty object; CONFIG_ERROR when the lock is unavailable or the write
// fails.
Status mutate_json_file(const std::filesystem::path& path,
    const std::function<bool(Json::Value&)>& mutate);

} // namespace imza
