#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
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
#include "network/json.h"
#include "network/network.h"
#include "providers/pricing.h"
#include "runtime/modal_queue.h"
#include "workspace/attachments.h"
#include "workspace/skills.h"

namespace imza {

// A compaction summary absorbs the prior context; the previous summary
// rides along under the earlier-compactions tag so nothing is lost.
inline std::string fold_compaction_summary(
    std::string previous, std::string summary)
{
    if (previous.empty()) {
        return summary;
    }
    summary.insert(0, previous + "\n\n<earlier-compactions>\n");
    summary += "\n</earlier-compactions>";
    return summary;
}

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
    // Wall-clock finish time; epoch 0 when the turn predates tracking.
    std::chrono::system_clock::time_point finished_at { };
    // Every provider request this turn made (a tool loop produces several).
    std::vector<UsageEntry> usage;
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
        std::optional<JsonValue> return_value;
        std::vector<DiffView> diffs;
        // One entry per accepted file mutation, in call order, for undo.
        std::vector<FileChange> changes;
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

// A session-level event with no model turn behind it (a working-directory
// change). Rendered dim in the timeline and replayed into model history as
// a tagged user message so the agent sees it as a discrete event.
struct Notice {
    std::string text;
};

using ConversationItem = std::variant<UserTurn, AssistantTurn, ToolCall,
    TodoList, CompactionEvent, Notice>;

// 0-based index of the final UserTurn in `items`, or nullopt when there
// is none; shared by the compaction boundary scans.
std::optional<std::size_t> last_user_turn_index(
    const std::vector<ConversationItem>& items);

// One user submission's usage: everything the agent spent between two
// user messages (the whole tool loop, across model rounds, including
// subagent work folded into the spawning turn). Usage recorded before
// the first user message forms its own leading group.
struct SubmissionUsage {
    std::string model;
    std::chrono::system_clock::time_point finished_at { };
    std::uint64_t tokens = 0;
    double cost          = 0.0;
};

// Groups a transcript into per-submission usage rows, in order. Groups
// with no usage are dropped; the model and finish time label each group
// from its last assistant turn.
std::vector<SubmissionUsage> submission_usage(
    const std::vector<ConversationItem>& items);

struct UnsavedSession { };

struct PersistedSession {
    std::filesystem::path path;
};

using SessionPersistence = std::variant<UnsavedSession, PersistedSession>;

// One entry in the session's ordered document sequence: a plan revision,
// a build log, or a handoff report. Plan accessors operate on the last
// PLAN entry; build decisions append to the last BUILD entry or start a new
// one; each handoff report is appended as its own HANDOFF entry.
struct SessionDoc {
    enum class Kind { PLAN, BUILD, HANDOFF };
    Kind kind = Kind::PLAN;
    std::string content;
};

// One user annotation on the plan document, pinned to a 1-based document
// line. Section and anchor text derive from the document at revise time.
struct PlanNote {
    std::size_t line = 0;
    std::string body;
};

// Hard cap on one plan document, applied to both the stored object and
// any future model-facing rendering of it.
inline constexpr std::size_t MAX_PLAN_BYTES = 16 * 1024;

// Undo stack bounds; the oldest entries are evicted first.
inline constexpr std::size_t MAX_UNDO_STEPS = 200;
inline constexpr std::size_t MAX_UNDO_BYTES = 16 * 1024 * 1024;

struct SessionSnapshot {
    std::string title;
    std::vector<ConversationItem> items;
    TodoList todo;
    std::vector<SessionDoc> documents;
    std::string compacted_summary;
    std::size_t compacted_item_count = 0;
    bool plan_mode                   = true;
    SessionPersistence persistence   = UnsavedSession { };
    Usage totals;
    Usage last;
    double total_cost = 0.0;
    std::vector<UsageEntry> usage_log;
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

// A submission waiting on skill approvals before its turn may start.
struct PendingSkillTurn {
    std::string text;
    std::vector<Attachment> attachments;
    std::vector<Skill> awaiting;
    std::size_t next = 0;
};

class Session final : public Subsystem {
public:
    enum class Phase { IDLE, CONNECTING, STREAMING, AWAITING };
    using Mode = SessionMode;
    struct Countdown {
        std::chrono::steady_clock::time_point deadline;
        bool stalled = false;
        // The full wait the deadline was derived from, so the overlay can
        // draw the remaining fraction as a bar.
        std::chrono::milliseconds ttl { 0 };
    };
    struct StatusView {
        Mode mode;
        Usage totals;
        Usage last;
        double total_cost;
    };

    Session() = default;
    // Abandons pending modal promises so blocked workers unblock.
    ~Session() { queue.abandon(); }

    // Snapshots: safe to read without the session lock.
    std::shared_ptr<const std::vector<ConversationItem>> items() const;
    std::shared_ptr<const TodoList> todo() const;
    std::shared_ptr<const std::vector<QueuedMessage>> queued() const;
    std::shared_ptr<const std::vector<SessionDoc>> documents() const;
    ModalPayload modal() const;
    // modal() deep-copies the payload; the probes below exist for per-frame
    // render paths instead.
    std::size_t modal_kind() const;
    std::shared_ptr<const ModalPayload> modal_handle() const;
    std::uint64_t modal_serial() const;
    std::uint64_t content_serial() const;
    std::string session_id() const;
    Phase phase() const;
    Mode mode() const;
    std::string error() const;
    std::string connect_status() const;
    std::string title() const;
    std::vector<std::string> attachment_names() const;
    std::string plan_doc() const;
    bool has_plan() const;
    std::optional<Countdown> retry_countdown() const;
    Usage last() const;
    std::optional<std::chrono::milliseconds> turn_elapsed() const;
    StatusView status_view() const;
    bool has_items() const;
    bool has_pending_work() const;
    // True while any CompactionEvent is still running; drives the status
    // bar spinner without scanning the timeline.
    bool compaction_running() const
    {
        std::lock_guard lock(_mutex);
        return _running_compactions > 0;
    }
    SessionSnapshot snapshot() const;
    std::optional<SessionSnapshot> snapshot_for_save() const;
    void restore(SessionSnapshot snapshot);
    void set_persistence(SessionPersistence persistence);

    // One entry per provider request this conversation made, including
    // subagent work folded in.
    std::vector<UsageEntry> usage_log() const;
    // Folds finished entries in (a subagent's log) without re-computing
    // cost.
    void add_usage(std::vector<UsageEntry> entries);
    // Moves the log out; handing entries to the parent this way cannot
    // double-count them.
    std::vector<UsageEntry> drain_usage();
    // Provider id stamped on usage entries recorded for the current turn.
    void set_entry_provider(std::string provider);

    void set_mode(Mode next_mode);
    void set_error(std::string msg);
    void clear_error();
    // Optional sink invoked (outside the lock) whenever set_error publishes a
    // message; the composition root wires it to the notification store so the
    // error surfaces app-wide. Empty means no sink.
    using ErrorSink = std::function<void(std::string)>;
    void set_error_sink(ErrorSink sink);
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
    void append_notice(std::string text);
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

    // Undo stack, newest last. Conversation-scoped: restore() and a fresh
    // session start empty, and it is never persisted.
    void push_undo(std::vector<FileChange> changes);
    // The newest entry without removing it, so the caller can check for
    // drift before committing to the revert.
    std::optional<FileChange> peek_undo() const;
    std::optional<FileChange> pop_undo();
    // Empties the stack oldest-first; a subagent's parent folds the child's
    // changes in this way.
    std::vector<FileChange> drain_undo();
    std::size_t undo_depth() const;
    // A popped entry parked while its inverse waits on a permission prompt.
    std::optional<FileChange> pending_undo() const;
    void set_pending_undo(std::optional<FileChange> change);
    void clear_undo();
    void set_tool_subagents(
        const ToolCallRequest& req, std::vector<std::size_t> ids);
    void set_tool_subagent_chats(
        const ToolCallRequest& req, std::vector<SubagentChat> chats);
    void set_todo(TodoList todo);
    std::string create_plan(std::string content);
    std::string edit_plan(
        const std::string& old, const std::string& fresh, std::size_t count);
    // Append one markdown entry to the current build log, starting a new
    // build document when the last document is a plan revision. Never
    // errors; the returned string is always empty (mirrors create_plan's
    // shape so the binding can report failures uniformly).
    std::string append_build_decision(std::string entry);
    // Append a handoff report as a new document, so reports accumulate in
    // session order and a later one reflects the latest stint rather than
    // overwriting an earlier one.
    std::string append_handoff_report(std::string content);
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

    // User dialogs and agent requests awaiting presentation.
    ModalQueue queue;

    // Loaded skills and the pending skill turn: conversation-scoped, so
    // they vanish with the conversation (clear on restore, /new, chdir).
    bool is_skill_loaded(const std::filesystem::path& path) const;
    // Records a loaded skill's rendered body; empty `error` on success.
    bool load_skill(const Skill& skill, std::string& error);
    void record_skill_load(const std::filesystem::path& path, std::string body);
    std::string skill_prompt_suffix() const;
    // The loaded skills' directories (SKILL.md parents), sorted.
    std::vector<std::filesystem::path> loaded_skill_directories() const;
    void clear_loaded_skills();
    std::optional<PendingSkillTurn> pending_skill_turn() const;
    void set_pending_skill_turn(PendingSkillTurn turn);
    std::optional<PendingSkillTurn> advance_pending_skill_turn();
    std::optional<PendingSkillTurn> take_pending_skill_turn();
    // Fires when the loaded set changes. Callbacks run on the publishing
    // thread; keep the Subscription.
    [[nodiscard]] Signal<>::Subscription subscribe_to_skills_change(
        Signal<>::Callback callback);
    // Exit confirmation overlay: a full-screen dialog that supersedes any
    // presented modal without disturbing it. Presentation state only.
    bool exit_confirm() const;
    void set_exit_confirm(bool open);
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
    [[nodiscard]] Signal<>::Subscription subscribe_to_documents_change(
        Signal<>::Callback callback);
    [[nodiscard]] Signal<>::Subscription subscribe_to_compaction_change(
        Signal<>::Callback callback);

private:
    AssistantTurn* _last_assistant_locked();
    const AssistantTurn* _last_assistant_locked() const;
    SessionSnapshot _build_snapshot() const;
    CompactionEvent* _find_compaction_locked(std::size_t id);
    // Marks the compaction `id` complete/failed, decrements the running
    // count, and publishes; false when no event carries the id.
    bool _complete_compaction_locked(
        std::size_t id, CompactionEvent::Status status);
    ToolCall* _find_tool_locked(
        const ToolCallRequest& req, bool unfinished_only);
    void _push_undo_locked(std::vector<FileChange> changes);
    ToolCall* _find_planning_tool_locked(const ToolCallRequest& req);
    void _finalize_reasoning(AssistantTurn& a);
    void _finish_session_locked(const std::string& error);
    // Fill unfinished tool calls with a cancelled result so an
    // interrupted turn cannot block compaction or switching forever.
    void _cancel_dangling_tools_locked();
    void _update_usage(
        const StreamEvent& usage_event, const ModelPricing& pricing);
    void _notify_title_change();
    // Copy-on-write: detach when a snapshot still holds the container.
    static void _ensure_unique(
        std::shared_ptr<std::vector<ConversationItem>>& items);
    static void _ensure_unique(std::shared_ptr<TodoList>& todo);
    static void _ensure_unique(
        std::shared_ptr<std::vector<QueuedMessage>>& queued);
    static void _ensure_unique(
        std::shared_ptr<std::vector<SessionDoc>>& documents);

    mutable std::mutex _mutex;

    std::shared_ptr<std::vector<ConversationItem>> _items
        = std::make_shared<std::vector<ConversationItem>>();
    ModalPayload _modal = std::monostate { };
    // Shared snapshot backing modal_handle(); refreshed under _mutex
    // wherever _modal is assigned.
    std::shared_ptr<const ModalPayload> _modal_handle
        = std::make_shared<const ModalPayload>();
    bool _exit_confirm            = false;
    std::uint64_t _modal_serial   = 0;
    std::uint64_t _content_serial = 0;
    Phase _phase                  = Phase::IDLE;
    Mode _mode                    = Mode::PLAN;
    std::string _error;
    ErrorSink _error_sink;
    std::string _connect_status;
    std::string _title;
    bool _title_generation_claimed = false;

    std::shared_ptr<TodoList> _todo = std::make_shared<TodoList>();
    std::shared_ptr<std::vector<SessionDoc>> _documents
        = std::make_shared<std::vector<SessionDoc>>();
    // Bumped on every plan mutation; _plan_seen_version is the version the
    // agent last read. A mismatch rejects imza.plan.edit so the agent
    // cannot patch content it has not seen.
    std::size_t _plan_version      = 0;
    std::size_t _plan_seen_version = 0;
    // Version of the plan the build context last received as a submission
    // message; 0 means the plan was never submitted.
    std::size_t _plan_submitted_version = 0;
    std::shared_ptr<std::vector<QueuedMessage>> _queued
        = std::make_shared<std::vector<QueuedMessage>>();
    std::set<std::string> _loaded_skills;
    std::map<std::string, std::string> _skill_contents;
    std::optional<PendingSkillTurn> _pending_skill_turn;
    Signal<> _skills_changed;

    std::optional<Countdown> _retry_countdown;

    Usage _totals;
    Usage _last;
    double _total_cost = 0.0;
    std::vector<UsageEntry> _usage_log;
    std::string _entry_provider;

    std::size_t _next_tool_id        = 1;
    std::size_t _next_compaction_id  = 1;
    std::size_t _running_compactions = 0;
    std::size_t _next_queued_id      = 0;
    std::optional<std::chrono::steady_clock::time_point> _reasoning_start;
    std::optional<std::chrono::steady_clock::time_point> _turn_started;
    std::atomic<bool> _interrupt_requested { false };

    std::vector<FileChange> _undo;
    std::size_t _undo_bytes = 0;
    std::optional<FileChange> _pending_undo;

    std::string _compacted_summary;
    std::size_t _compacted_item_count = 0;
    SessionPersistence _persistence   = UnsavedSession { };
    bool _dirty                       = false;
    std::string _session_id           = unique_session_id();

    Signal<> _mode_changed;
    Signal<> _title_changed;
    Signal<> _attachments_changed;
    Signal<> _documents_changed;
    Signal<> _compaction_changed;
};

enum class WorkflowPhase { PLAN, BUILD, REVIEW };

WorkflowPhase next_workflow_phase(WorkflowPhase phase, bool review_available);
WorkflowPhase previous_workflow_phase(
    WorkflowPhase phase, bool review_available);
std::optional<Session::Mode> workflow_mode(WorkflowPhase phase);

using WorkflowFn         = std::function<WorkflowPhase()>;
using WorkflowNavigateFn = std::function<void(WorkflowPhase)>;

} // namespace imza
