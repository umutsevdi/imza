#include "workspace/review.h"
#include "common/util.h"
#include "platform/command_runner.h"

#include <json/reader.h>
#include <json/value.h>
#include <json/writer.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>

namespace imza {
namespace {

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
            ReviewFile file;
            file.new_path = path;
            file.kind     = ReviewFile::Kind::UNTRACKED;
            ReviewHunk hunk;
            hunk.header         = "@@ -0,0 +1 @@";
            hunk.new_start      = 1;
            std::size_t line_no = 1;
            std::string line;
            while (std::getline(input, line)) {
                if (line.find('\0') != std::string::npos) {
                    file.kind      = ReviewFile::Kind::BINARY;
                    file.additions = 0;
                    hunk.lines.clear();
                    break;
                }
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                hunk.lines.push_back(ReviewLine { ReviewLine::Kind::ADDITION,
                    std::nullopt, line_no++, std::move(line) });
                ++file.additions;
            }
            if (file.kind == ReviewFile::Kind::BINARY) {
                review.files.push_back(std::move(file));
                continue;
            }
            hunk.new_count = file.additions;
            file.hunks.push_back(std::move(hunk));
            review.files.push_back(std::move(file));
        }
    }

    std::string review_path(const ReviewFile& file)
    {
        return file.new_path.empty() ? file.old_path : file.new_path;
    }

    std::string format_review_patch(const RepositoryReview& review)
    {
        std::string patch;
        for (const ReviewFile& file : review.files) {
            const std::string old_path
                = file.old_path.empty() ? file.new_path : file.old_path;
            const std::string new_path
                = file.new_path.empty() ? file.old_path : file.new_path;
            patch += "diff --git a/" + old_path + " b/" + new_path + "\n";
            if (file.kind == ReviewFile::Kind::BINARY) {
                patch += "Binary files a/" + old_path + " and b/" + new_path
                    + " differ\n";
                continue;
            }
            patch += "--- "
                + (file.kind == ReviewFile::Kind::ADDED
                            || file.kind == ReviewFile::Kind::UNTRACKED
                        ? std::string("/dev/null")
                        : "a/" + old_path)
                + "\n";
            patch += "+++ "
                + (file.kind == ReviewFile::Kind::DELETED
                        ? std::string("/dev/null")
                        : "b/" + new_path)
                + "\n";
            for (const ReviewHunk& hunk : file.hunks) {
                patch += hunk.header + "\n";
                for (const ReviewLine& line : hunk.lines) {
                    switch (line.kind) {
                    case ReviewLine::Kind::CONTEXT: patch += ' '; break;
                    case ReviewLine::Kind::ADDITION: patch += '+'; break;
                    case ReviewLine::Kind::DELETION: patch += '-'; break;
                    case ReviewLine::Kind::META: break;
                    }
                    patch += line.content + "\n";
                }
            }
        }
        return patch;
    }

    std::string normalized_comment_body(std::string_view body)
    {
        if (body.size() >= 5 && body[0] == '[' && body[1] == 'P'
            && body[2] >= '0' && body[2] <= '3' && body[3] == ']'
            && body[4] == ' ') {
            body.remove_prefix(5);
        }
        std::string normalized;
        bool pending_space = false;
        for (const unsigned char ch : body) {
            if (std::isspace(ch)) {
                pending_space = !normalized.empty();
                continue;
            }
            if (pending_space) {
                normalized += ' ';
            }
            normalized += static_cast<char>(std::tolower(ch));
            pending_space = false;
        }
        return normalized;
    }

    std::optional<ReviewLineAnchor> resolve_finding_anchor(
        const RepositoryReview& review, std::string_view requested_path,
        std::string_view side, std::size_t line_number)
    {
        for (const ReviewFile& file : review.files) {
            if (requested_path != file.old_path
                && requested_path != file.new_path) {
                continue;
            }
            for (const ReviewHunk& hunk : file.hunks) {
                for (const ReviewLine& line : hunk.lines) {
                    const bool matches = side == "new"
                        ? line.kind == ReviewLine::Kind::ADDITION
                            && line.new_line == line_number
                        : line.kind == ReviewLine::Kind::DELETION
                            && line.old_line == line_number;
                    if (matches) {
                        return ReviewLineAnchor { review_path(file),
                            line.old_line, line.new_line, line.content };
                    }
                }
            }
        }
        return std::nullopt;
    }

} // namespace

ReviewLoadResult load_repository_review(const std::filesystem::path& root)
{
    const std::optional<std::string> diff
        = git_working_diff(root, { .renames = true });
    if (!diff.has_value()) {
        return "Git diff could not be loaded.";
    }
    ReviewLoadResult parsed = parse_git_diff(*diff);
    auto* review            = std::get_if<RepositoryReview>(&parsed);
    if (review == nullptr) {
        return parsed;
    }
    const std::string prefix = "git -C " + shell_quote(root);
    const CommandResult untracked
        = run_command(prefix + " ls-files --others --exclude-standard",
            std::chrono::seconds { 5 });
    if (untracked.spawned && !untracked.timed_out && untracked.exit_code == 0) {
        append_untracked(*review, root, untracked.output);
    }
    return parsed;
}

std::string format_review_plan_prompt(
    std::string_view instructions, const std::vector<ReviewComment>& comments)
{
    if (comments.empty()) {
        return { };
    }
    std::string prompt(instructions);
    for (const ReviewComment& comment : comments) {
        const std::string line = comment.anchor.new_line
            ? std::to_string(*comment.anchor.new_line)
            : comment.anchor.old_line ? std::to_string(*comment.anchor.old_line)
                                      : "?";
        prompt += "\n\n- `" + comment.anchor.file + ":" + line + "`";
        if (comment.stale) {
            prompt += " (stale)";
        }
        prompt += "\n  ";
        for (const char c : comment.body) {
            prompt += c;
            if (c == '\n') {
                prompt += "  ";
            }
        }
    }
    return prompt;
}

std::string format_ai_review_prompt(std::string_view instructions,
    const RepositoryReview& review, const std::vector<ReviewComment>& comments)
{
    Json::Value input(Json::objectValue);
    input["diff"] = format_review_patch(review);
    Json::Value existing(Json::arrayValue);
    for (const ReviewComment& comment : comments) {
        Json::Value value(Json::objectValue);
        value["file"] = comment.anchor.file;
        if (comment.anchor.new_line) {
            value["side"] = "new";
            value["line"] = static_cast<Json::UInt64>(*comment.anchor.new_line);
        } else if (comment.anchor.old_line) {
            value["side"] = "old";
            value["line"] = static_cast<Json::UInt64>(*comment.anchor.old_line);
        }
        value["body"] = comment.body;
        existing.append(std::move(value));
    }
    input["existing_comments"] = std::move(existing);
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return std::string(instructions) + "\n\nInput JSON:\n"
        + Json::writeString(writer, input);
}

AiReviewParseResult parse_ai_review_response(
    std::string_view response, const RepositoryReview& review)
{
    const std::size_t begin = response.find('{');
    const std::size_t end   = response.rfind('}');
    if (begin == std::string_view::npos || end == std::string_view::npos
        || begin > end) {
        return "AI review returned invalid JSON.";
    }
    const std::string json(response.substr(begin, end - begin + 1));
    Json::CharReaderBuilder reader;
    Json::Value root;
    std::string errors;
    std::istringstream stream(json);
    if (!Json::parseFromStream(reader, stream, &root, &errors)
        || !root.isObject() || !root["findings"].isArray()) {
        return "AI review returned an invalid findings document.";
    }

    std::vector<ReviewCommentDraft> comments;
    bool rejected = false;
    for (const Json::Value& finding : root["findings"]) {
        if (!finding.isObject() || !finding["file"].isString()
            || !finding["side"].isString() || !finding["line"].isUInt64()
            || !finding["severity"].isString() || !finding["body"].isString()) {
            rejected = true;
            continue;
        }
        const std::string file     = finding["file"].asString();
        const std::string side     = finding["side"].asString();
        const std::string severity = finding["severity"].asString();
        const std::string body     = finding["body"].asString();
        const Json::UInt64 line    = finding["line"].asUInt64();
        if ((side != "old" && side != "new")
            || (severity != "P0" && severity != "P1" && severity != "P2"
                && severity != "P3")
            || line == 0 || body.empty()) {
            rejected = true;
            continue;
        }
        const auto anchor = resolve_finding_anchor(
            review, file, side, static_cast<std::size_t>(line));
        if (!anchor) {
            rejected = true;
            continue;
        }
        ReviewCommentDraft comment { *anchor, "[" + severity + "] " + body };
        const bool duplicate = std::ranges::any_of(
            comments, [&comment](const ReviewCommentDraft& candidate) {
                return candidate.anchor == comment.anchor
                    && normalized_comment_body(candidate.body)
                    == normalized_comment_body(comment.body);
            });
        if (!duplicate) {
            comments.push_back(std::move(comment));
        }
    }
    if (comments.empty() && rejected && !root["findings"].empty()) {
        return "AI review did not reference any valid changed lines.";
    }
    return comments;
}

ReviewState::Snapshot::Snapshot()
    : review(std::make_shared<const RepositoryReview>())
{
}

ReviewState::Snapshot ReviewState::snapshot() const
{
    std::lock_guard lock(_mutex);
    return _state;
}

std::vector<ReviewComment> ReviewState::comments() const
{
    std::lock_guard lock(_mutex);
    return _state.comments;
}

void ReviewState::set_loading()
{
    {
        std::lock_guard lock(_mutex);
        _state.status = LoadStatus::LOADING;
        _state.error.clear();
        ++_state.generation;
    }
    _publish();
}

void ReviewState::set_result(ReviewLoadResult result)
{
    {
        std::lock_guard lock(_mutex);
        if (auto* review = std::get_if<RepositoryReview>(&result)) {
            auto next
                = std::make_shared<const RepositoryReview>(std::move(*review));
            for (ReviewComment& comment : _state.comments) {
                comment.stale = true;
                for (const ReviewFile& file : next->files) {
                    const std::string& path
                        = file.new_path.empty() ? file.old_path : file.new_path;
                    if (path != comment.anchor.file) {
                        continue;
                    }
                    for (const ReviewHunk& hunk : file.hunks) {
                        if (std::ranges::any_of(
                                hunk.lines, [&comment](const ReviewLine& line) {
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
                    if (!comment.stale) {
                        break;
                    }
                }
            }
            _state.review = std::move(next);
            _state.status = LoadStatus::LOADED;
            _state.error.clear();
        } else {
            _state.status = LoadStatus::ERROR;
            _state.error  = std::move(std::get<std::string>(result));
        }
        ++_state.generation;
    }
    _publish();
}

std::size_t ReviewState::add_comment(ReviewLineAnchor anchor, std::string body)
{
    std::size_t id;
    {
        std::lock_guard lock(_mutex);
        id = next_comment_id_++;
        _state.comments.push_back(
            ReviewComment { id, std::move(anchor), std::move(body), false });
        ++_state.generation;
    }
    _publish();
    return id;
}

std::size_t ReviewState::add_comments(std::vector<ReviewCommentDraft> comments)
{
    std::size_t added = 0;
    {
        std::lock_guard lock(_mutex);
        for (ReviewCommentDraft& comment : comments) {
            const bool duplicate = std::ranges::any_of(
                _state.comments, [&comment](const ReviewComment& candidate) {
                    return candidate.anchor == comment.anchor
                        && normalized_comment_body(candidate.body)
                        == normalized_comment_body(comment.body);
                });
            if (duplicate) {
                continue;
            }
            _state.comments.push_back(ReviewComment { next_comment_id_++,
                std::move(comment.anchor), std::move(comment.body), false });
            ++added;
        }
        if (added > 0) {
            ++_state.generation;
        }
    }
    if (added > 0) {
        _publish();
    }
    return added;
}

void ReviewState::update_comment(std::size_t id, std::string body)
{
    {
        std::lock_guard lock(_mutex);
        const auto it
            = std::ranges::find(_state.comments, id, &ReviewComment::id);
        if (it == _state.comments.end()) {
            return;
        }
        it->body = std::move(body);
        ++_state.generation;
    }
    _publish();
}

void ReviewState::delete_comment(std::size_t id)
{
    {
        std::lock_guard lock(_mutex);
        std::erase_if(_state.comments,
            [id](const ReviewComment& comment) { return comment.id == id; });
        ++_state.generation;
    }
    _publish();
}

void ReviewState::request_jump(std::size_t id)
{
    {
        std::lock_guard lock(_mutex);
        _state.jump_comment = id;
        ++_state.generation;
    }
    _publish();
}

void ReviewState::clear_jump()
{
    std::lock_guard lock(_mutex);
    _state.jump_comment.reset();
}

void ReviewState::request_file_jump(std::string path)
{
    {
        std::lock_guard lock(_mutex);
        _state.jump_file = std::move(path);
        ++_state.generation;
    }
    _publish();
}

void ReviewState::clear_file_jump()
{
    std::lock_guard lock(_mutex);
    _state.jump_file.reset();
}

void ReviewState::clear_comments()
{
    {
        std::lock_guard lock(_mutex);
        if (_state.comments.empty()) {
            return;
        }
        _state.comments.clear();
        _state.jump_comment.reset();
        ++_state.generation;
    }
    _publish();
}

Signal<>::Subscription ReviewState::subscribe(Signal<>::Callback callback)
{
    return changed_.subscribe(std::move(callback));
}

void ReviewState::_publish() { changed_.publish(); }

} // namespace imza
