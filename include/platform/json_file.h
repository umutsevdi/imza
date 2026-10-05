#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "common/types.h"
#include "network/json.h"

namespace imza {

// Writes pre-serialized JSON to path via a .tmp sibling + rename. Creates
// the parent directory. Callers pick compact or pretty serialization.
Status write_json_file(
    const std::filesystem::path& path, std::string_view serialized);

std::optional<std::string> read_text_file(const std::filesystem::path& path);

// Reads a file into the dynamic JSON value. nullopt when absent, unreadable,
// or malformed; check existence first when absent must differ from malformed.
std::optional<JsonValue> read_json_file(const std::filesystem::path& path);

// Sidecar-locked read-modify-write: absent or malformed input becomes an
// empty object; CONFIG_ERROR when the lock is unavailable or the write
// fails.
Status mutate_json_file(const std::filesystem::path& path,
    const std::function<bool(JsonValue&)>& mutate);

} // namespace imza
