#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "common/tool_call.h"

namespace imza {

// Text file I/O behind the lua tool.file.* bindings. load_text rejects
// missing paths, directories, and binary content; save_text truncates.
bool load_text(const std::string& path, std::string& out, std::string& err);
bool save_text(
    const std::string& path, const std::string& content, std::string& err);

// Pure content transforms. Each returns the new content or an error;
// the caller decides whether to persist. Semantics match the lua
// tool.file.* bindings:
//   insert: 1-based line; the text goes before that line, pushing it
//     down. line == 0 or line == length+1 appends; beyond is an error.
//   replace: first `count` occurrences of `old`; count == 0 = all.
//     Empty `old` or no match is an error.
std::optional<std::string> insert_text(const std::string& content,
    const std::string& text, std::size_t line, std::string& err);
std::optional<std::string> replace_text(const std::string& content,
    const std::string& old, const std::string& fresh, std::size_t count,
    std::string& err);

// Diff of two file versions as localized hunks: patience diff with a
// Myers fallback computes the edit script, hunks separated by fewer than
// 2*context unchanged lines are merged, and large gaps collapse into
// DiffRow::Kind::SKIP elision rows. Used for the lua aggregate per-file
// diffs.
DiffView make_diff_view(const std::string& path,
    const std::vector<std::string>& old_lines,
    const std::vector<std::string>& new_lines);

} // namespace imza
