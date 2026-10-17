#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <string_view>

namespace imza {

// The one file walk the tree operations share (imza.fs.grep, imza.fs.list,
// imza.fs.ts_query, attachment discovery).
struct FileWalkOptions {
    std::filesystem::path root;
    // Set false to search a directory the caller named explicitly: a
    // deliberate target is never silently skipped.
    bool apply_ignores = true;
    // 0 means no limit.
    std::size_t limit = 0;
    // A default-constructed point means no deadline.
    std::chrono::steady_clock::time_point deadline { };
};

// Calls `visit` with each regular file under `options.root`, as a path
// relative to that root. Returning false from `visit` stops the walk.
// `truncated` is set when the limit or deadline stopped it early, so a
// caller can distinguish "no more matches" from "stopped looking".
void for_each_file(const FileWalkOptions& options,
    const std::function<bool(const std::filesystem::path&)>& visit,
    bool& truncated);

// Directory names the fallback walk never descends into. A heuristic
// used only when git enumeration is unavailable; it does not read
// .gitignore, so a pattern that is not a directory name is not honored.
bool ignored_directory(std::string_view name);

} // namespace imza
