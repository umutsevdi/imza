#pragma once

#include <json/json.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "common/imza_signal.h"
#include "common/modal.h"
#include "common/tool_call.h"
#include "common/types.h"
#include "common/util.h"
#include "network/chat.h"
#include "network/network.h"
#include "providers/pricing.h"
#include "workspace/attachments.h"

namespace imza {

struct UserTurn {
    std::string text;
    std::vector<Attachment> attachments;
};

struct AssistantTurn {
    std::string markdown;
    std::string reasoning;
    std::string reasoning_signature;
    std::optional<std::chrono::milliseconds> reasoning_ms;
    std::string model;
    std::string reasoning_effort;
};

struct SubagentChat {
    std::string title;
    std::string transcript;
};

struct ToolCall {
    enum class Phase { PLANNING, EXECUTING };
    struct Result {
        enum class Kind { OUTPUT, ERROR, REJECT, CANCEL };
        Kind kind;
        std::string text;
        std::optional<Json::Value> return_value;
        std::vector<DiffView> diffs;
        std::vector<CanvasView> canvases;
        std::optional<ShellStatus> shell_status;
        std::vector<LuaBindingCall> dispatch_log;
    };
    std::size_t id = 0;
    std::string call_id;
    std::string name;
    std::string args;
    std::vector<std::size_t> subagent_ids;
    std::vector<SubagentChat> subagent_chats;
    std::optional<Result> result;
    Phase phase = Phase::EXECUTING;
};

struct CompactionEvent {
    enum class Status { RUNNING, COMPLETED, FAILED };
    std::size_t id = 0;
    Status status  = Status::RUNNING;
};

using ConversationItem = std::variant<UserTurn, AssistantTurn, ToolCall,
    TodoList, ModalAnswer, CompactionEvent>;

// 0-based index of the final UserTurn in `items`, or nullopt when there
// is none; shared by the compaction boundary scans.
std::optional<std::size_t> last_user_turn_index(
    const std::vector<ConversationItem>& items);

struct UnsavedSession { };

struct PersistedSession {
    std::filesystem::path path;
};

using SessionPersistence = std::variant<UnsavedSession, PersistedSession>;

struct PlanDoc {
    std::string content;

    bool operator==(const PlanDoc&) const = default;
};

// One user annotation on the plan document, pinned to a 1-based document
// line. Section and anchor text derive from the document at revise time.
struct PlanNote {
    std::size_t line = 0;
    std::string body;

    bool operator==(const PlanNote&) const = default;
};

// Hard cap on one plan document, applied to both the stored object and
// any future model-facing rendering of it.
inline constexpr std::size_t MAX_PLAN_BYTES = 16 * 1024;

struct SessionSnapshot {
    std::string title;
    std::vector<ConversationItem> items;
    TodoList todo;
    std::vector<PlanDoc> plans;
    std::string compacted_summary;
    std::size_t compacted_item_count = 0;
    bool plan_mode                   = true;
    SessionPersistence persistence   = UnsavedSession { };
};

struct LoadedSession {
    SessionSnapshot snapshot;
    std::filesystem::path workspace;
};

struct QueuedMessage {
    std::size_t id;
    std::string text;
    std::vector<Attachment> attachments;
};

class Session final : public ApplicationComponent {
public:
    enum class Phase { IDLE, CONNECTING, STREAMING, AWAITING };
    using Mode = SessionMode;
    struct Countdown {
        std::chrono::steady_clock::time_point deadline;
        bool stalled = false;
    };
    struct StatusView {
        Mode mode;
        Usage totals;
        Usage last;
        double total_cost;
    };

    Session() = default;

    const std::vector<ConversationItem>& items() const { return _items; }
    ModalPayload modal() const;
    std::uint64_t modal_serial() const;
    std::uint64_t content_serial() const;
    std::string session_id() const;
    Phase phase() const;
    Mode mode() const;
    std::string error() const;
    std::string connect_status() const;
    std::string title() const;
    std::vector<std::string> attachment_names() const;
    const TodoList& todo() const { return _todo; }
    const std::vector<QueuedMessage>& queued() const { return _queued; }
    const std::vector<PlanDoc>& plans() const { return _plans; }
    std::string plan_doc() const;
    std::optional<Countdown> retry_countdown() const;
    Usage last() const;
    std::optional<std::chrono::milliseconds> turn_elapsed() const;
    StatusView status_view() const;
    bool has_items() const;
    bool has_pending_work() const;
    SessionSnapshot snapshot() const;
    std::optional<SessionSnapshot> snapshot_for_save() const;
    void restore(SessionSnapshot snapshot);
    void set_persistence(SessionPersistence persistence);

    void set_mode(Mode next_mode);
    void set_error(std::string msg);
    void clear_error();
    void set_connect_status(std::string status);
    bool claim_title_generation();
    void set_title(std::string title);
    void cancel_queued(std::size_t id);
    void enqueue_message(
        std::string text, std::vector<Attachment> attachments = { });
    std::optional<QueuedMessage> pop_queued();

    void begin_send(
        std::string text, std::vector<Attachment> attachments = { });
    void append_assistant(
        std::string model = "", std::string reasoning_effort = "");
    void set_last_assistant_metadata(
        std::string model, std::string reasoning_effort);
    void append_item(ConversationItem item);
    std::pair<std::size_t, std::size_t> begin_compaction();
    void finish_compaction(std::size_t id, std::string summary,
        std::size_t compacted_item_count, bool success);
    // Manual /compact completion: completes `id`, folds `summary` into
    // the compacted context (preceded by the previous summary when one
    // exists), and advances the boundary by `absorbed_items` so
    // build_history skips what the summary covers.
    void complete_manual_compaction(
        std::size_t id, std::string summary, std::size_t absorbed_items);
    void fill_tool_result(const ToolCallRequest& req, ToolCall::Result result);
    void set_tool_subagents(
        const ToolCallRequest& req, std::vector<std::size_t> ids);
    void set_tool_subagent_chats(
        const ToolCallRequest& req, std::vector<SubagentChat> chats);
    void set_todo(TodoList todo);
    std::string create_plan(std::string content);
    std::string edit_plan(
        const std::string& old, const std::string& fresh, std::size_t count);
    void mark_plan_seen();
    // Returns the size-capped plan submission message for the first build
    // turn of a stint (or after a plan revision) and consumes the pending
    // submission; nullopt when there is nothing to submit. Not const: the
    // watermark sync shares the session lock with the mode check.
    std::optional<std::string> plan_submission_for_build();
    void set_modal(ModalPayload payload);
    void clear_modal();
    void bump_modal_serial();
    void present_modal(ModalPayload payload);
    void set_phase(Phase phase);
    void mark_retry(int wait_seconds, bool stalled = false);

    void apply(const StreamEvent& ev, const ModelPricing& pricing);
    bool finish_session(std::string error);
    std::vector<Message> build_history(std::string_view system_prompt,
        ApiStandard dialect = ApiStandard::OPENAI) const;

    std::optional<AssistantTurn> last_assistant() const;
    void reset_reasoning();

    void request_interrupt();
    void clear_interrupt();
    bool interrupt_requested() const;

    [[nodiscard]] Signal<>::Subscription subscribe_to_mode_change(
        Signal<>::Callback callback);
    [[nodiscard]] Signal<>::Subscription subscribe_to_title_change(
        Signal<>::Callback callback);
    [[nodiscard]] Signal<>::Subscription subscribe_to_attachments_change(
        Signal<>::Callback callback);
    [[nodiscard]] Signal<>::Subscription subscribe_to_plan_change(
        Signal<>::Callback callback);

private:
    AssistantTurn* _last_assistant_locked();
    const AssistantTurn* _last_assistant_locked() const;
    SessionSnapshot _build_snapshot() const;
    CompactionEvent* _find_compaction_locked(std::size_t id);
    ToolCall* _find_tool_locked(
        const ToolCallRequest& req, bool unfinished_only);
    ToolCall* _find_planning_tool_locked(const ToolCallRequest& req);
    void _finalize_reasoning(AssistantTurn& a);
    void _finish_session_locked(const std::string& error);
    void _update_usage(
        const StreamEvent& usage_event, const ModelPricing& pricing);
    void _notify_title_change();

    mutable std::mutex _mutex;

    std::vector<ConversationItem> _items;
    ModalPayload _modal           = std::monostate { };
    std::uint64_t _modal_serial   = 0;
    std::uint64_t _content_serial = 0;
    Phase _phase                  = Phase::IDLE;
    Mode _mode                    = Mode::PLAN;
    std::string _error;
    std::string _connect_status;
    std::string _title;
    bool _title_generation_claimed = false;

    TodoList _todo;
    std::vector<PlanDoc> _plans;
    // Bumped on every plan mutation; _plan_seen_version is the version the
    // agent last read. A mismatch rejects imza.plan.edit so the agent
    // cannot patch content it has not seen.
    std::size_t _plan_version      = 0;
    std::size_t _plan_seen_version = 0;
    // Version of the plan the build context last received as a submission
    // message; 0 means the plan was never submitted.
    std::size_t _plan_submitted_version = 0;
    std::vector<QueuedMessage> _queued;

    std::optional<Countdown> _retry_countdown;

    Usage _totals;
    Usage _last;
    double _total_cost = 0.0;

    std::size_t _next_tool_id       = 1;
    std::size_t _next_compaction_id = 1;
    std::size_t _next_queued_id     = 0;
    std::optional<std::chrono::steady_clock::time_point> _reasoning_start;
    std::optional<std::chrono::steady_clock::time_point> _turn_started;
    std::atomic<bool> _interrupt_requested { false };

    std::string _compacted_summary;
    std::size_t _compacted_item_count = 0;
    SessionPersistence _persistence   = UnsavedSession { };
    bool _dirty                       = false;
    std::string _session_id           = unique_session_id();

    Signal<> _mode_changed;
    Signal<> _title_changed;
    Signal<> _attachments_changed;
    Signal<> _plan_changed;
};

enum class WorkflowPhase { PLAN, BUILD, REVIEW };

WorkflowPhase next_workflow_phase(WorkflowPhase phase, bool review_available);
WorkflowPhase previous_workflow_phase(
    WorkflowPhase phase, bool review_available);
std::optional<Session::Mode> workflow_mode(WorkflowPhase phase);

using WorkflowFn         = std::function<WorkflowPhase()>;
using WorkflowNavigateFn = std::function<void(WorkflowPhase)>;

} // namespace imza
