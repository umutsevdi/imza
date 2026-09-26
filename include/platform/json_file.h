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

// Reads a file completely; nullopt when it cannot be opened or read.
std::optional<std::string> read_text_file(const std::filesystem::path& path);

// Reads and parses a JSON document; nullopt when the file cannot be opened
// or does not parse. Callers distinguish absent from malformed by checking
// existence first when the difference matters.
std::optional<Json::Value> read_json_file(const std::filesystem::path& path);

// Lock-and-merge mutation of a JSON store: acquire the sidecar lock, read
// the newest document from disk (absent or malformed becomes an empty
// object), hand it to mutate, and replace the file when mutate returns
// true. CONFIG_ERROR when the lock is unavailable or the write fails.
Status mutate_json_file(const std::filesystem::path& path,
    const std::function<bool(Json::Value&)>& mutate);

} // namespace imza
