#pragma once

#include <algorithm>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "common/util.h"

namespace imza {

// Resolves a path to a registry entry by exact filename first, then by
// extension, both case-insensitive. `Entry` is any type exposing
// `extensions` and `filenames` as spans of string_view (both the tools'
// LanguageEntry and the UI's LanguageDefinition qualify). Shared so the two
// generated registries cannot diverge in matching behavior.
template <typename Entry>
const Entry* match_language_entry(
    std::span<const Entry> entries, std::string_view path)
{
    const std::filesystem::path file(path);
    const std::string filename = to_lower(file.filename().string());
    for (const Entry& entry : entries) {
        if (std::ranges::find(entry.filenames, filename)
            != entry.filenames.end()) {
            return &entry;
        }
    }
    std::string extension = to_lower(file.extension().string());
    if (!extension.empty() && extension.front() == '.') {
        extension.erase(0, 1);
    }
    for (const Entry& entry : entries) {
        if (std::ranges::find(entry.extensions, extension)
            != entry.extensions.end()) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace imza
