#pragma once

#include <string>

namespace imza {

// Text file I/O behind the lua imza.fs.* bindings. load_text rejects
// missing paths, directories, and binary content; save_text creates
// missing parent directories and truncates.
bool load_text(const std::string& path, std::string& out, std::string& err);
bool save_text(
    const std::string& path, const std::string& content, std::string& err);

// Creates `path` and every missing parent. Reached only after permission
// authorization.
bool make_directories(const std::string& path, std::string& err);

// Removes a file, or a directory only when it is empty. Reached only after
// permission authorization.
bool remove_path(const std::string& path, std::string& err);

// Copies a regular file from `source` to `target`, creating missing
// parents. When `move` is set the source is removed after a successful
// copy, so a failure never loses the original. Reached only after
// permission authorization of the destination.
bool transfer_file(const std::string& source, const std::string& target,
    bool move, std::string& err);

// Pure content transforms (insert_text, replace_text, make_diff_view)
// live in common/text_edit.h.

} // namespace imza
