#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace imza {

struct ChangedFile {
    enum class Kind {
        MODIFIED,
        ADDED,
        UNTRACKED,
        DELETED,
        RENAMED,
        COPIED,
        CONFLICTED,
        UNKNOWN,
    };

    std::string path;
    Kind kind = Kind::UNKNOWN;

    bool operator==(const ChangedFile&) const = default;
};

std::vector<ChangedFile> parse_git_status(std::string_view status);
std::string normalize_git_branch(std::string_view branch);

// Cheap fingerprint of the repository metadata that changes when the user
// stages work or moves the branch: the `(mtime, size)` of the git directory's
// `index` and `HEAD`. Resolves a `.git` pointer file (worktree, submodule) to
// its real git directory. Nullopt when no repository or neither file exists;
// the caller then falls back to a full refresh.
std::optional<std::uint64_t> git_metadata_stamp(
    const std::filesystem::path& root);

struct ChangeSummary {
    std::size_t additions   = 0;
    std::size_t deletions   = 0;
    std::uint64_t signature = 0;

    bool operator==(const ChangeSummary&) const = default;
};

ChangeSummary summarize_git_diff(std::string_view diff);
void summarize_untracked_files(const std::filesystem::path& root,
    const std::vector<ChangedFile>& files, ChangeSummary& summary);

struct GitDiffOptions {
    bool renames = false;
    bool numstat = false; // prepend added/deleted counts before the patch
};

std::optional<std::string> git_working_diff(
    const std::filesystem::path& root, const GitDiffOptions& options);

struct RepositoryState {
    std::string branch;
    std::vector<ChangedFile> changed_files;
    ChangeSummary changes;
};

struct ReviewLine {
    enum class Kind { CONTEXT, ADDITION, DELETION, META };

    Kind kind = Kind::CONTEXT;
    std::optional<std::size_t> old_line;
    std::optional<std::size_t> new_line;
    std::string content;
};

struct ReviewHunk {
    std::string header;
    std::vector<ReviewLine> lines;
    std::size_t old_start = 0;
    std::size_t old_count = 0;
    std::size_t new_start = 0;
    std::size_t new_count = 0;
};

struct ReviewFile {
    enum class Kind {
        MODIFIED,
        ADDED,
        DELETED,
        RENAMED,
        COPIED,
        UNTRACKED,
        BINARY,
    };

    std::string old_path;
    std::string new_path;
    Kind kind             = Kind::MODIFIED;
    std::size_t additions = 0;
    std::size_t deletions = 0;
    std::vector<ReviewHunk> hunks;
};

struct RepositoryReview {
    std::vector<ReviewFile> files;
};

using ReviewLoadResult = std::variant<RepositoryReview, std::string>;

ReviewLoadResult parse_git_diff(std::string_view patch);

} // namespace imza
