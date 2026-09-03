#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "core/git.h"
#include "common/ursa_signal.h"

namespace ursa {

struct ReviewLineAnchor {
    std::string file;
    std::optional<std::size_t> old_line;
    std::optional<std::size_t> new_line;
    std::string content;

    bool operator==(const ReviewLineAnchor&) const = default;
};

struct ReviewComment {
    std::size_t id = 0;
    ReviewLineAnchor anchor;
    std::string body;
    bool stale = false;
};

struct ReviewCommentDraft {
    ReviewLineAnchor anchor;
    std::string body;
};

using AiReviewParseResult
    = std::variant<std::vector<ReviewCommentDraft>, std::string>;

std::string format_review_plan_prompt(
    const std::vector<ReviewComment>& comments);
std::string format_ai_review_prompt(
    const RepositoryReview& review, const std::vector<ReviewComment>& comments);
AiReviewParseResult parse_ai_review_response(
    std::string_view response, const RepositoryReview& review);
ReviewLoadResult load_repository_review(const std::filesystem::path& root);

class ReviewState {
public:
    enum class LoadStatus { IDLE, LOADING, LOADED, ERROR };

    struct Snapshot {
        Snapshot();

        LoadStatus status = LoadStatus::IDLE;
        std::shared_ptr<const RepositoryReview> review;
        std::vector<ReviewComment> comments;
        std::string error;
        std::optional<std::size_t> jump_comment;
        std::optional<std::string> jump_file;
        std::uint64_t generation = 0;
    };

    struct CommentsSnapshot {
        std::vector<ReviewComment> comments;
        std::uint64_t generation = 0;
    };

    Snapshot snapshot() const;
    CommentsSnapshot comments_snapshot() const;
    void set_loading();
    void set_result(ReviewLoadResult result);
    std::size_t add_comment(ReviewLineAnchor anchor, std::string body);
    std::size_t add_comments(std::vector<ReviewCommentDraft> comments);
    void update_comment(std::size_t id, std::string body);
    void delete_comment(std::size_t id);
    void request_jump(std::size_t id);
    void clear_jump();
    void request_file_jump(std::string path);
    void clear_file_jump();
    void clear_comments();
    [[nodiscard]] Signal<>::Subscription subscribe(Signal<>::Callback callback);

private:
    void _publish();
    mutable std::mutex mutex_;
    Snapshot state_;
    std::size_t next_comment_id_ = 1;
    Signal<> changed_;
};

} // namespace ursa
