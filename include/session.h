#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <json/json.h>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "network.h"
#include "attachments.h"
#include "signal.h"
#include "types.h"

namespace ursa {

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

struct ToolCall {
    struct Result {
        enum class Kind { OUTPUT, ERROR, REJECT, CANCEL };
        Kind kind;
        std::string text;
        std::optional<DiffView> diff { };
        std::optional<ShellStatus> shell_status { };
    };
    std::size_t id = 0;
    std::string call_id;
    std::string name;
    std::string args;
    std::optional<Result> result;
};

struct TodoItem {
    enum class Status { PENDING, IN_PROGRESS, COMPLETED, CANCELLED };
    std::string content;
    Status status = Status::PENDING;
};

struct TodoList {
    std::vector<TodoItem> items;
};

struct CompactionEvent {
    enum class Status { RUNNING, COMPLETED, FAILED };
    std::size_t id = 0;
    Status status = Status::RUNNING;
};

using ConversationItem
    = std::variant<UserTurn, AssistantTurn, ToolCall, TodoList, ModalAnswer,
        CompactionEvent>;

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
    bool plan_mode = true;
    SessionPersistence persistence = UnsavedSession { };
};

struct ConnectModal {
    enum class Entry { MANAGE, PICK_MODEL };
    Entry entry = Entry::MANAGE;
};

struct ViewerModal {
    std::string title;
    std::string content;
    std::string lang;
    std::size_t start_line = 1;
    bool line_numbers      = true;
    std::string metadata;
};

struct VariantModal {
    std::vector<std::string> options;
    std::string current;
};

struct SessionsModal {
    std::vector<std::string> titles;
    std::vector<std::string> saved_at;
    std::vector<std::string> paths;
};

struct SkillsModal {
    struct Entry {
        std::string name;
        std::string description;
        std::string project_root;
        SkillPolicy policy = SkillPolicy::ASK;
    };
    std::vector<Entry> entries;
};

using ModalPayload = std::variant<std::monostate, ViewerModal,
    ToolCallRequest, QuestionForm, ConnectModal, VariantModal, SessionsModal,
    SkillsModal>;

struct QueuedMessage {
    std::size_t id;
    std::string text;
    std::vector<FileAttachment> attachments;
};

std::optional<TodoList> parse_todo_args(const Json::Value& args);
std::string todo_summary(const TodoList& todo);
std::string tool_result_text(const ToolCall& call);
std::string denial_text(const std::string& reason);

class Session {
public:
    enum class Phase { IDLE, CONNECTING, STREAMING, AWAITING };
    enum class Mode { PLAN, BUILD };
    struct Countdown {
        std::chrono::steady_clock::time_point deadline;
    };
    struct StatusView {
        Mode mode;
        Usage totals;
        Usage last;
        double total_cost;
    };

    Session();

    const std::vector<ConversationItem>& items() const { return items_; }
    ModalPayload modal() const;
    std::uint64_t modal_serial() const;
    std::uint64_t content_serial() const;
    Phase phase() const;
    Mode mode() const;
    std::string error() const;
    std::string connect_status() const;
    std::string title() const;
    std::vector<std::string> attachment_names() const;
    const TodoList& todo() const { return todo_; }
    const std::vector<QueuedMessage>& queued() const { return queued_; }
    std::optional<Countdown> retry_countdown() const;
    Usage totals() const;
    Usage last() const;
    double total_cost() const;
    double last_cost() const;
    StatusView status_view() const;
    bool has_pending_work() const;
    SessionSnapshot snapshot() const;
    void restore(SessionSnapshot snapshot);
    void set_persistence(SessionPersistence persistence);

    void toggle_mode();
    void set_error(std::string msg);
    void clear_error();
    void set_connect_status(std::string status);
    bool claim_title_generation();
    void set_title(std::string title);
    void cancel_queued(std::size_t id);
    void enqueue_message(std::string text,
        std::vector<FileAttachment> attachments = { });
    std::optional<QueuedMessage> pop_queued();

    void begin_send(std::string text,
        std::vector<FileAttachment> attachments = { });
    void append_assistant(std::string model = "", std::string reasoning_effort = "");
    void set_last_assistant_metadata(std::string model,
        std::string reasoning_effort);
    void append_item(ConversationItem item);
    std::pair<std::size_t, std::size_t> begin_compaction();
    void finish_compaction(std::size_t id, std::string summary,
        std::size_t compacted_item_count, bool success);
    void append_tool(const ToolCallRequest& req);
    void fill_tool_result(const ToolCallRequest& req, ToolCall::Result result);
    void set_todo(TodoList todo);
    void set_modal(ModalPayload payload);
    void clear_modal();
    void bump_modal_serial();
    void present_modal(ModalPayload payload);
    void set_phase(Phase phase);
    void mark_retry(int wait_seconds);

    void apply(const StreamEvent& ev, const ModelPricing& pricing);
    bool finish_session(std::string error);
    std::vector<Message> build_history(
        std::string_view system_prompt, ApiStandard dialect = ApiStandard::OPENAI) const;

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
    void finalize_reasoning(AssistantTurn& a);
    void finish_session_locked(const std::string& error);
    void update_usage(const StreamEvent& usage_event, const ModelPricing& pricing);
    void _notify_title_change();

    mutable std::mutex mutex_;

    std::vector<ConversationItem> items_;
    ModalPayload modal_         = std::monostate { };
    std::uint64_t modal_serial_ = 0;
    std::uint64_t content_serial_ = 0;
    Phase phase_                = Phase::IDLE;
    Mode mode_                  = Mode::PLAN;
    std::string error_;
    std::string connect_status_;
    std::string title_;
    bool title_generation_claimed_ = false;

    TodoList todo_;
    std::vector<QueuedMessage> queued_;

    std::optional<Countdown> retry_countdown_;

    Usage totals_;
    Usage last_;
    double total_cost_ = 0.0;
    double last_cost_  = 0.0;

    std::size_t next_tool_id_   = 1;
    std::size_t next_compaction_id_ = 1;
    std::size_t next_queued_id_ = 0;
    std::optional<std::chrono::steady_clock::time_point> reasoning_start_;
    std::atomic<bool> interrupt_requested_ { false };

    std::string compacted_summary_;
    std::size_t compacted_item_count_ = 0;
    SessionPersistence persistence_ = UnsavedSession { };

    Signal<> title_changed_;
    Signal<> attachments_changed_;
};

} // namespace ursa
