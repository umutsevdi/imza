#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ursa {

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

struct ChangeSummary {
    std::size_t additions   = 0;
    std::size_t deletions   = 0;
    std::uint64_t signature = 0;

    bool operator==(const ChangeSummary&) const = default;
};

ChangeSummary summarize_git_diff(std::string_view diff);

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

} // namespace ursa
