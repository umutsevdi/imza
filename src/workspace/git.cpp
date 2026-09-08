#include "workspace/git.h"

#include "common/util.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>

namespace ursa {

namespace {

    constexpr std::uint64_t FNV_OFFSET = 14695981039346656037ULL;
    constexpr std::uint64_t FNV_PRIME  = 1099511628211ULL;

    void hash_bytes(std::uint64_t& signature, std::string_view value)
    {
        for (const unsigned char byte : value) {
            signature ^= byte;
            signature *= FNV_PRIME;
        }
    }

    bool has_status(std::string_view status, char value)
    {
        return status.find(value) != std::string_view::npos;
    }

    ChangedFile::Kind changed_file_kind(std::string_view status)
    {
        if (has_status(status, 'U') || status == "AA" || status == "DD") {
            return ChangedFile::Kind::CONFLICTED;
        }
        if (has_status(status, 'D')) {
            return ChangedFile::Kind::DELETED;
        }
        if (has_status(status, 'R')) {
            return ChangedFile::Kind::RENAMED;
        }
        if (has_status(status, 'C')) {
            return ChangedFile::Kind::COPIED;
        }
        if (status == "??") {
            return ChangedFile::Kind::UNTRACKED;
        }
        if (has_status(status, 'A')) {
            return ChangedFile::Kind::ADDED;
        }
        if (has_status(status, 'M') || has_status(status, 'T')) {
            return ChangedFile::Kind::MODIFIED;
        }
        return ChangedFile::Kind::UNKNOWN;
    }

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

std::vector<ChangedFile> parse_git_status(std::string_view status)
{
    std::vector<ChangedFile> files;
    for (std::string line : split_lines(status)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.size() < 4 || line[2] != ' ') {
            continue;
        }
        files.push_back(ChangedFile { line.substr(3),
            changed_file_kind(std::string_view(line).substr(0, 2)) });
    }
    return files;
}

std::string normalize_git_branch(std::string_view branch)
{
    return std::string(trim(branch));
}

ChangeSummary summarize_git_diff(std::string_view diff)
{
    ChangeSummary summary;
    summary.signature = FNV_OFFSET;
    hash_bytes(summary.signature, diff);
    std::size_t line_start = 0;
    while (line_start < diff.size()) {
        const std::size_t line_end  = diff.find('\n', line_start);
        const std::string_view line = diff.substr(line_start,
            line_end == std::string_view::npos ? diff.size() - line_start
                                               : line_end - line_start);
        line_start
            = line_end == std::string_view::npos ? diff.size() : line_end + 1;
        if (line.empty()) {
            break;
        }
        const std::size_t first_tab = line.find('\t');
        if (first_tab == std::string_view::npos) {
            break;
        }
        const std::size_t second_tab = line.find('\t', first_tab + 1);
        if (second_tab == std::string_view::npos) {
            break;
        }
        std::size_t additions        = 0;
        std::size_t deletions        = 0;
        const std::string_view added = line.substr(0, first_tab);
        const std::string_view deleted
            = line.substr(first_tab + 1, second_tab - first_tab - 1);
        const auto added_result = std::from_chars(
            added.data(), added.data() + added.size(), additions);
        const auto deleted_result = std::from_chars(
            deleted.data(), deleted.data() + deleted.size(), deletions);
        if (added_result.ec == std::errc { }
            && deleted_result.ec == std::errc { }) {
            summary.additions += additions;
            summary.deletions += deletions;
        }
    }
    return summary;
}

void summarize_untracked_files(const std::filesystem::path& root,
    const std::vector<ChangedFile>& files, ChangeSummary& summary)
{
    for (const ChangedFile& file : files) {
        if (file.kind != ChangedFile::Kind::UNTRACKED) {
            continue;
        }
        hash_bytes(summary.signature, file.path);
        summary.signature ^= 0xFF;
        summary.signature *= FNV_PRIME;
        std::ifstream input(root / file.path, std::ios::binary);
        if (!input) {
            continue;
        }
        std::array<char, 8192> buffer;
        std::size_t lines = 0;
        bool binary       = false;
        bool has_content  = false;
        char last         = '\0';
        while (input) {
            input.read(buffer.data(), buffer.size());
            const std::streamsize count = input.gcount();
            if (count <= 0) {
                continue;
            }
            const std::string_view chunk(
                buffer.data(), static_cast<std::size_t>(count));
            hash_bytes(summary.signature, chunk);
            has_content = true;
            last        = chunk.back();
            binary      = binary || chunk.find('\0') != std::string_view::npos;
            lines += std::ranges::count(chunk, '\n');
        }
        if (!binary) {
            if (has_content && last != '\n') {
                ++lines;
            }
            summary.additions += lines;
        }
    }
}

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
