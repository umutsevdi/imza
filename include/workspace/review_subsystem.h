#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "common/imza_signal.h"
#include "common/types.h"
#include "workspace/git.h"

namespace imza {

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
    std::string_view instructions, const std::vector<ReviewComment>& comments);
// The repository review rendered as a unified patch; shared by the AI
// review and handoff prompts.
std::string format_review_patch(const RepositoryReview& review);
std::string format_ai_review_prompt(std::string_view instructions,
    const RepositoryReview& review, const std::vector<ReviewComment>& comments);
AiReviewParseResult parse_ai_review_response(
    std::string_view response, const RepositoryReview& review);
ReviewLoadResult load_repository_review(const std::filesystem::path& root);

// A hidden review-pass subagent: AI findings on the diff, or the handoff
// report. Both run the same way, so their running state shares one shape.
enum class ReviewPass { FINDINGS, HANDOFF };

struct ReviewPassState {
    std::optional<std::size_t> task_id;
};

class ReviewSubsystem final : public Subsystem {
public:
    enum class LoadStatus { IDLE, LOADING, LOADED, ERROR };

    ReviewSubsystem() = default;
    ~ReviewSubsystem();

    struct Snapshot {
        Snapshot();

        LoadStatus status = LoadStatus::IDLE;
        std::shared_ptr<const RepositoryReview> review;
        std::vector<ReviewComment> comments;
        std::string error;
        std::optional<std::size_t> jump_comment;
        std::optional<std::string> jump_file;
        std::uint64_t generation = 0;
        // Review passes run off the UI thread and are reachable from any
        // workflow phase, so their progress lives here rather than in the
        // Review component.
        ReviewPassState findings;
        ReviewPassState handoff;
    };

    Snapshot snapshot() const;
    std::vector<ReviewComment> comments() const;
    // The repository diff is fetched only for a visible pane.
    bool active() const;
    // Requests arriving while inactive are replayed on the next activation.
    void set_active(bool active);
    // Loads the repository review for `root` on a component-owned worker.
    // Coalesces: a request arriving mid-load reruns once with the newest
    // root. Empty `root` is a no-op. The tab starts no thread.
    void reload(std::filesystem::path root);
    // Stops and joins the loader; called from the composition root.
    void stop() override;
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
    void set_pass_task(ReviewPass pass, std::optional<std::size_t> task_id);
    [[nodiscard]] Signal<>::Subscription subscribe_to_change(
        Signal<>::Callback callback);

private:
    void _publish();
    void _maybe_start_worker();
    mutable std::mutex _mutex;
    Snapshot _state;
    std::size_t _next_comment_id = 1;
    std::filesystem::path _reload_root;
    bool _reload_pending = false;
    bool _reload_running = false;
    bool _active         = false;
    std::jthread _worker;
    Signal<> _changed;
};

} // namespace imza
