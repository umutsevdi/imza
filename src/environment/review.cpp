#include "review.h"

#include "command_runner.h"
#include "util.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <sstream>

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
    std::size_t split = body.rfind(" b/");
    if (split == std::string_view::npos) {
        split = body.rfind("\"b/");
    }
    if (split == std::string_view::npos) {
        return { };
    }
    std::string path(body.substr(split + 1));
    if (!path.empty() && path.front() == '"') path.erase(0, 1);
    if (!path.empty() && path.back() == '"') path.pop_back();
    return strip_prefix(std::move(path));
}

bool parse_range(std::string_view value, std::size_t& start,
    std::size_t& count)
{
    const auto start_result
        = std::from_chars(value.data(), value.data() + value.size(), start);
    if (start_result.ec != std::errc { }) return false;
    count = 1;
    if (start_result.ptr == value.data() + value.size()
        || *start_result.ptr != ',') {
        return true;
    }
    const char* count_begin = start_result.ptr + 1;
    const auto count_result = std::from_chars(
        count_begin, value.data() + value.size(), count);
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

std::string quote_arg(const std::filesystem::path& path)
{
    std::string value = path.string();
    std::string out = "\"";
    for (const char ch : value) {
        if (ch == '"') {
            out += "\\\"";
        } else {
            out += ch;
        }
    }
    return out + "\"";
}

void append_untracked(RepositoryReview& review,
    const std::filesystem::path& root, std::string_view paths)
{
    for (const std::string& path : split_lines(paths)) {
        if (path.empty()) {
            continue;
        }
        std::ifstream input(root / path, std::ios::binary);
        if (!input) {
            continue;
        }
        std::string content { std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>() };
        ReviewFile file;
        file.new_path = path;
        file.kind = ReviewFile::Kind::UNTRACKED;
        if (content.find('\0') != std::string::npos) {
            file.kind = ReviewFile::Kind::BINARY;
            review.files.push_back(std::move(file));
            continue;
        }
        ReviewHunk hunk;
        hunk.header = "@@ -0,0 +1 @@";
        hunk.new_start = 1;
        std::size_t line_no = 1;
        std::istringstream stream(content);
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            hunk.lines.push_back(ReviewLine { ReviewLine::Kind::ADDITION,
                std::nullopt, line_no++, std::move(line) });
            ++file.additions;
        }
        hunk.new_count = file.additions;
        file.hunks.push_back(std::move(hunk));
        review.files.push_back(std::move(file));
    }
}

}

ReviewLoadResult parse_git_diff(std::string_view patch)
{
    RepositoryReview review;
    ReviewFile* file = nullptr;
    ReviewHunk* hunk = nullptr;
    std::size_t old_line = 0;
    std::size_t new_line = 0;

    for (std::string line : split_lines(patch)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.starts_with("diff --git ")) {
            review.files.push_back(ReviewFile { });
            file = &review.files.back();
            file->new_path = header_path(line);
            file->old_path = file->new_path;
            hunk = nullptr;
            continue;
        }
        if (file == nullptr) {
            if (!line.empty()) {
                return "invalid git patch: content before file header";
            }
            continue;
        }
        if (line.starts_with("new file mode ")) {
            file->kind = ReviewFile::Kind::ADDED;
        } else if (line.starts_with("deleted file mode ")) {
            file->kind = ReviewFile::Kind::DELETED;
        } else if (line.starts_with("rename from ")) {
            file->kind = ReviewFile::Kind::RENAMED;
            file->old_path = line.substr(12);
        } else if (line.starts_with("rename to ")) {
            file->new_path = line.substr(10);
        } else if (line.starts_with("copy from ")) {
            file->kind = ReviewFile::Kind::COPIED;
            file->old_path = line.substr(10);
        } else if (line.starts_with("copy to ")) {
            file->new_path = line.substr(8);
        } else if (line.starts_with("Binary files ")
            || line.starts_with("GIT binary patch")) {
            file->kind = ReviewFile::Kind::BINARY;
        } else if (line.starts_with("--- ")) {
            const std::string path = line.substr(4);
            if (path != "/dev/null") {
                file->old_path = strip_prefix(path);
            }
        } else if (line.starts_with("+++ ")) {
            const std::string path = line.substr(4);
            if (path != "/dev/null") {
                file->new_path = strip_prefix(path);
            }
        } else if (line.starts_with("@@ ")) {
            file->hunks.push_back(ReviewHunk { line, { } });
            hunk = &file->hunks.back();
            if (!parse_hunk_header(line, *hunk)) {
                return "invalid git patch: malformed hunk header";
            }
            old_line = hunk->old_start;
            new_line = hunk->new_start;
        } else if (hunk != nullptr && !line.empty()) {
            if (line.front() == '+') {
                hunk->lines.push_back(ReviewLine { ReviewLine::Kind::ADDITION,
                    std::nullopt, new_line++, line.substr(1) });
                ++file->additions;
            } else if (line.front() == '-') {
                hunk->lines.push_back(ReviewLine { ReviewLine::Kind::DELETION,
                    old_line++, std::nullopt, line.substr(1) });
                ++file->deletions;
            } else if (line.front() == ' ') {
                hunk->lines.push_back(ReviewLine { ReviewLine::Kind::CONTEXT,
                    old_line++, new_line++, line.substr(1) });
            } else if (line.front() == '\\') {
                hunk->lines.push_back(ReviewLine { ReviewLine::Kind::META,
                    std::nullopt, std::nullopt, line });
            }
        }
    }
    return review;
}

ReviewLoadResult load_repository_review(const std::filesystem::path& root)
{
    const std::string prefix = "git -C " + quote_arg(root);
    CommandResult diff = run_command(prefix
            + " diff --no-ext-diff --no-color --find-renames --find-copies HEAD --",
        std::chrono::seconds { 10 });
    if (!diff.spawned || diff.timed_out) {
        return "git diff could not be loaded";
    }
    if (diff.exit_code != 0) {
        diff = run_command(prefix
                + " diff --cached --no-ext-diff --no-color --find-renames --find-copies --",
            std::chrono::seconds { 10 });
    }
    if (!diff.spawned || diff.timed_out || diff.exit_code != 0) {
        return diff.output.empty() ? "git diff failed" : diff.output;
    }
    ReviewLoadResult parsed = parse_git_diff(diff.output);
    auto* review = std::get_if<RepositoryReview>(&parsed);
    if (review == nullptr) {
        return parsed;
    }
    const CommandResult untracked = run_command(
        prefix + " ls-files --others --exclude-standard",
        std::chrono::seconds { 5 });
    if (untracked.spawned && !untracked.timed_out && untracked.exit_code == 0) {
        append_untracked(*review, root, untracked.output);
    }
    return parsed;
}

ReviewState::Snapshot ReviewState::snapshot() const
{
    std::lock_guard lock(mutex_);
    return state_;
}

void ReviewState::set_loading()
{
    {
        std::lock_guard lock(mutex_);
        state_.status = LoadStatus::LOADING;
        state_.error.clear();
    }
    _publish();
}

void ReviewState::set_result(ReviewLoadResult result)
{
    {
        std::lock_guard lock(mutex_);
        if (auto* review = std::get_if<RepositoryReview>(&result)) {
            state_.review = std::move(*review);
            for (ReviewComment& comment : state_.comments) {
                comment.stale = true;
                for (const ReviewFile& file : state_.review.files) {
                    const std::string& path = file.new_path.empty()
                        ? file.old_path
                        : file.new_path;
                    if (path != comment.anchor.file) continue;
                    for (const ReviewHunk& hunk : file.hunks) {
                        if (std::ranges::any_of(hunk.lines,
                                [&comment](const ReviewLine& line) {
                                    return line.old_line
                                            == comment.anchor.old_line
                                        && line.new_line
                                            == comment.anchor.new_line
                                        && line.content
                                            == comment.anchor.content;
                                })) {
                            comment.stale = false;
                            break;
                        }
                    }
                    if (!comment.stale) break;
                }
            }
            state_.status = LoadStatus::LOADED;
            state_.error.clear();
        } else {
            state_.status = LoadStatus::ERROR;
            state_.error = std::move(std::get<std::string>(result));
        }
    }
    _publish();
}

std::size_t ReviewState::add_comment(
    ReviewLineAnchor anchor, std::string body)
{
    std::size_t id;
    {
        std::lock_guard lock(mutex_);
        id = next_comment_id_++;
        state_.comments.push_back(
            ReviewComment { id, std::move(anchor), std::move(body), false });
    }
    _publish();
    return id;
}

void ReviewState::update_comment(std::size_t id, std::string body)
{
    {
        std::lock_guard lock(mutex_);
        const auto it = std::ranges::find(state_.comments, id,
            &ReviewComment::id);
        if (it == state_.comments.end()) return;
        it->body = std::move(body);
    }
    _publish();
}

void ReviewState::delete_comment(std::size_t id)
{
    {
        std::lock_guard lock(mutex_);
        std::erase_if(state_.comments,
            [id](const ReviewComment& comment) { return comment.id == id; });
    }
    _publish();
}

void ReviewState::request_jump(std::size_t id)
{
    {
        std::lock_guard lock(mutex_);
        state_.jump_comment = id;
    }
    _publish();
}

void ReviewState::clear_jump()
{
    std::lock_guard lock(mutex_);
    state_.jump_comment.reset();
}

Signal<>::Subscription ReviewState::subscribe(Signal<>::Callback callback)
{
    return changed_.subscribe(std::move(callback));
}

void ReviewState::_publish() { changed_.publish(); }

}
