#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "environment.h"
#include "network.h"
#include "tools.h"
#include "types.h"

namespace ursa {

struct SlashCommand {
    std::string name;
    std::string desc;
    enum class Action { EXIT, HELP, SETTINGS, DEMO, SKILL, SYSTEM_PROMPT };
    Action action = Action::SKILL;
};

std::vector<SlashCommand> slash_commands(const Config& cfg);
const SlashCommand* find_command(
    const std::vector<SlashCommand>& commands, std::string_view name);

struct LayoutCtx {
    enum class Kind { WIDE, NARROW };
    Kind kind;
    int width;
};

struct UserTurn {
    std::string text;
};

struct AssistantTurn {
    std::string markdown;
};

struct ToolCall {
    struct Result {
        enum class Kind { OUTPUT, ERROR, REJECT, CANCEL };
        Kind kind;
        std::string text;
    };
    std::size_t id = 0;
    std::string call_id { };
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

std::optional<TodoList> parse_todo_args(const Json::Value& args);
std::string todo_summary(const TodoList& todo);

struct ChangedFile {
    std::string path;
    std::string status;
};

using ConversationItem
    = std::variant<UserTurn, AssistantTurn, ToolCall, TodoList, ModalAnswer>;

struct SettingsModal {
    std::string model;
};

struct HelpModal { };

struct SystemPromptModal {
    std::string prompt;
};

using ModalPayload = std::variant<std::monostate, SettingsModal, HelpModal,
    SystemPromptModal, ToolCallRequest, QuestionForm>;

struct QueuedMessage {
    std::size_t id;
    std::string text;
};

struct UiState {
    enum class Phase { IDLE, STREAMING, AWAITING };
    enum class Mode { PLAN, BUILD };
    std::vector<ConversationItem> items;
    ModalPayload modal         = std::monostate { };
    std::uint64_t modal_serial = 0;
    Phase phase                = Phase::IDLE;
    Mode mode                  = Mode::PLAN;
    std::string error;

    TodoList todo;
    std::vector<ChangedFile> changed_files;
    std::vector<QueuedMessage> queued;
    bool env_ready = false;
    std::optional<std::string> agent_rules;
};

using PostFn = std::function<void(std::function<void()>)>;
using StreamFn
    = std::function<Status(const ChatRequest&, const StreamCallback&)>;

class Controller {
public:
    Controller(const Config& cfg, PostFn post, std::function<void()> on_exit,
        StreamFn stream_fn = { }, ToolRegistry tools = { },
        std::shared_future<Environment> env = { });
    ~Controller();

    Controller(const Controller&)            = delete;
    Controller& operator=(const Controller&) = delete;

    void submit(std::string text);
    void toggle_mode();
    void run_demo();
    void set_error(std::string msg);
    void close_modal();
    void resolve_modal(ModalResult result);
    void enqueue_user_modal(ModalPayload payload);
    void cancel_queued(std::size_t id);
    size_t queue_size() const;
    ModalResult request_modal(ModalPayload payload);
    UiState& state() { return state_; }
    const UiState& state() const { return state_; }
    const Config& config() const { return cfg_; }
    const std::vector<SlashCommand>& commands() const { return commands_; }
    std::string shell_name() const;

private:
    void submit_message(std::string text);
    void run_slash(std::string_view cmd);
    void apply(const StreamEvent& ev);
    void finish(std::string error);

    void _post(std::function<void()> f);
    std::vector<Message> _build_history() const;
    std::string _system_prompt() const;
    void _on_env_ready();
    void _spawn(std::vector<Message> history, StreamFn override);
    void _drive(std::vector<Message> history, StreamFn override);
    void _drain_pending_asks(std::vector<Message>& history,
        std::string& reply_buffer, const std::string& assistant_text);
    void _apply_tool_result(const ToolCallRequest& req, const ModalResult& res,
        std::vector<Message>& tool_msgs);
    void _apply_question_result(
        const ModalResult& res, std::string& reply_buffer);
    void _apply_ask_result(const ToolCallRequest& req, const ModalResult& res,
        std::vector<Message>& tool_msgs);
    void _run_tool(const ToolCallRequest& req, std::vector<Message>& tool_msgs);
    void _fill_tool_result(const ToolCallRequest& req, ToolCall::Result result);
    static std::string _tool_result_text(const ToolCall& call);
    void _present_front();
    void _enqueue_message(std::string text);
    void _drain_queued();

    Config cfg_;
    UiState state_;
    PostFn post_;
    std::function<void()> on_exit_;
    std::vector<SlashCommand> commands_;
    StreamFn stream_fn_;
    ToolRegistry tools_;

    std::shared_future<Environment> env_;
    std::atomic<bool> env_ready_ { false };
    std::optional<std::jthread> env_waiter_;

    struct PendingModal {
        ModalPayload payload;
        std::shared_ptr<std::promise<ModalResult>> promise;
    };
    std::deque<PendingModal> queue_;
    mutable std::mutex queue_mutex_;
    std::set<std::string> allowed_tools_;
    std::size_t next_tool_id_ = 1;
    std::size_t next_queued_id_ = 0;

    std::vector<StreamEvent> stream_events_;
    std::atomic<bool> alive_ { true };
    std::optional<std::jthread> worker_;
};

std::string error_text(Status st);

} // namespace ursa
