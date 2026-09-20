#pragma once

#include <json/json.h>

#include <atomic>
#include <chrono>
#include <cstdint>
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
    std::vector<FileAttachment> attachments;
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
    struct Result {
        enum class Kind { OUTPUT, ERROR, REJECT, CANCEL };
        Kind kind;
        std::string text;
        std::optional<DiffView> diff;
        std::vector<DiffView> diffs;
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
};

struct CompactionEvent {
    enum class Status { RUNNING, COMPLETED, FAILED };
    std::size_t id = 0;
    Status status  = Status::RUNNING;
};

using ConversationItem = std::variant<UserTurn, AssistantTurn, ToolCall,
    TodoList, ModalAnswer, CompactionEvent>;

struct UnsavedSession { };

struct PersistedSession {
    std::filesystem::path path;
};

using SessionPersistence = std::variant<UnsavedSession, PersistedSession>;

struct SessionSnapshot {
    std::string title;
    std::vector<ConversationItem> items;
    TodoList todo;
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
    std::vector<FileAttachment> attachments;
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
        std::string text, std::vector<FileAttachment> attachments = { });
    std::optional<QueuedMessage> pop_queued();

    void begin_send(
        std::string text, std::vector<FileAttachment> attachments = { });
    void append_assistant(
        std::string model = "", std::string reasoning_effort = "");
    void set_last_assistant_metadata(
        std::string model, std::string reasoning_effort);
    void append_item(ConversationItem item);
    std::pair<std::size_t, std::size_t> begin_compaction();
    void finish_compaction(std::size_t id, std::string summary,
        std::size_t compacted_item_count, bool success);
    void append_tool(const ToolCallRequest& req);
    void fill_tool_result(const ToolCallRequest& req, ToolCall::Result result);
    void set_tool_subagents(
        const ToolCallRequest& req, std::vector<std::size_t> ids);
    void set_tool_subagent_chats(
        const ToolCallRequest& req, std::vector<SubagentChat> chats);
    void set_todo(TodoList todo);
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

    [[nodiscard]] Signal<>::Subscription subscribe_to_title_change(
        Signal<>::Callback callback);
    [[nodiscard]] Signal<>::Subscription subscribe_to_attachments_change(
        Signal<>::Callback callback);

private:
    AssistantTurn* last_assistant_locked();
    const AssistantTurn* last_assistant_locked() const;
    ToolCall* _find_tool_locked(
        const ToolCallRequest& req, bool unfinished_only);
    void finalize_reasoning(AssistantTurn& a);
    void finish_session_locked(const std::string& error);
    void update_usage(
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

    Signal<> title_changed_;
    Signal<> attachments_changed_;
};

} // namespace imza
