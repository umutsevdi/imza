#include "core/git.h"

#include <charconv>

namespace ursa {

namespace {

    std::string strip_prefix(std::string path)
    {
        if (path.starts_with("a/") || path.starts_with("b/")) {
            path.erase(0, 2);
        }
        return path;
    }

    std::string header_path(std::string_view line)
    {
        constexpr std::string_view prefix = "diff --git ";
        if (!line.starts_with(prefix)) {
            return { };
        }
        const std::string_view body = line.substr(prefix.size());
        std::size_t split           = body.rfind(" b/");
        if (split == std::string_view::npos) {
            split = body.rfind("\"b/");
        }
        if (split == std::string_view::npos) {
            return { };
        }
        std::string path(body.substr(split + 1));
        if (!path.empty() && path.front() == '"')
            path.erase(0, 1);
        if (!path.empty() && path.back() == '"')
            path.pop_back();
        return strip_prefix(std::move(path));
    }

    bool parse_range(
        std::string_view value, std::size_t& start, std::size_t& count)
    {
        const auto start_result
            = std::from_chars(value.data(), value.data() + value.size(), start);
        if (start_result.ec != std::errc { })
            return false;
        count = 1;
        if (start_result.ptr == value.data() + value.size()
            || *start_result.ptr != ',') {
            return true;
        }
        const char* count_begin = start_result.ptr + 1;
        const auto count_result
            = std::from_chars(count_begin, value.data() + value.size(), count);
        return count_result.ec == std::errc { };
    }

    bool parse_hunk_header(std::string_view line, ReviewHunk& hunk)
    {
        const std::size_t minus = line.find('-');
        const std::size_t plus  = line.find('+', minus);
        if (minus == std::string_view::npos || plus == std::string_view::npos) {
            return false;
        }
        const std::string_view old_part = line.substr(minus + 1);
        const std::string_view new_part = line.substr(plus + 1);
        return parse_range(old_part, hunk.old_start, hunk.old_count)
            && parse_range(new_part, hunk.new_start, hunk.new_count);
    }

} // namespace

ReviewLoadResult parse_git_diff(std::string_view patch)
{
    RepositoryReview review;
    ReviewFile* file     = nullptr;
    ReviewHunk* hunk     = nullptr;
    std::size_t old_line = 0;
    std::size_t new_line = 0;

    std::size_t line_start = 0;
    while (line_start < patch.size()) {
        const std::size_t line_end = patch.find('\n', line_start);
        std::string_view line      = patch.substr(line_start,
            line_end == std::string_view::npos ? patch.size() - line_start
                                               : line_end - line_start);
        line_start
            = line_end == std::string_view::npos ? patch.size() : line_end + 1;
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (line.starts_with("diff --git ")) {
            review.files.push_back(ReviewFile { });
            file           = &review.files.back();
            file->new_path = header_path(line);
            file->old_path = file->new_path;
            hunk           = nullptr;
            continue;
        }
        if (file == nullptr) {
            if (!line.empty()) {
                return "Invalid git patch: content before file header.";
            }
            continue;
        }
        if (line.starts_with("new file mode ")) {
            file->kind = ReviewFile::Kind::ADDED;
        } else if (line.starts_with("deleted file mode ")) {
            file->kind = ReviewFile::Kind::DELETED;
        } else if (line.starts_with("rename from ")) {
            file->kind     = ReviewFile::Kind::RENAMED;
            file->old_path = std::string(line.substr(12));
        } else if (line.starts_with("rename to ")) {
            file->new_path = std::string(line.substr(10));
        } else if (line.starts_with("copy from ")) {
            file->kind     = ReviewFile::Kind::COPIED;
            file->old_path = std::string(line.substr(10));
        } else if (line.starts_with("copy to ")) {
            file->new_path = std::string(line.substr(8));
        } else if (line.starts_with("Binary files ")
            || line.starts_with("GIT binary patch")) {
            file->kind = ReviewFile::Kind::BINARY;
        } else if (line.starts_with("--- ")) {
            const std::string path(line.substr(4));
            if (path != "/dev/null") {
                file->old_path = strip_prefix(path);
            }
        } else if (line.starts_with("+++ ")) {
            const std::string path(line.substr(4));
            if (path != "/dev/null") {
                file->new_path = strip_prefix(path);
            }
        } else if (line.starts_with("@@ ")) {
            file->hunks.push_back(ReviewHunk { std::string(line), { } });
            hunk = &file->hunks.back();
            if (!parse_hunk_header(line, *hunk)) {
                return "Invalid git patch: malformed hunk header.";
            }
            old_line = hunk->old_start;
            new_line = hunk->new_start;
        } else if (hunk != nullptr && !line.empty()) {
            if (line.front() == '+') {
                hunk->lines.push_back(ReviewLine { ReviewLine::Kind::ADDITION,
                    std::nullopt, new_line++, std::string(line.substr(1)) });
                ++file->additions;
            } else if (line.front() == '-') {
                hunk->lines.push_back(ReviewLine { ReviewLine::Kind::DELETION,
                    old_line++, std::nullopt, std::string(line.substr(1)) });
                ++file->deletions;
            } else if (line.front() == ' ') {
                hunk->lines.push_back(ReviewLine { ReviewLine::Kind::CONTEXT,
                    old_line++, new_line++, std::string(line.substr(1)) });
            } else if (line.front() == '\\') {
                hunk->lines.push_back(ReviewLine { ReviewLine::Kind::META,
                    std::nullopt, std::nullopt, std::string(line) });
            }
        }
    }
    return review;
}

} // namespace ursa
