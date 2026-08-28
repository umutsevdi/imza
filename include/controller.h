#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "commands.h"
#include "network.h"
#include "provider_store.h"
#include "session.h"
#include "tools.h"
#include "types.h"

namespace ursa {

struct LayoutCtx {
    enum class Kind { WIDE, NARROW };
    static constexpr int wide_threshold = 100;
    static constexpr int panel_width    = 40;
    Kind kind;
    int width;
};

using PostFn = std::function<void(std::function<void()>)>;
using StreamFn
    = std::function<Status(const ChatRequest&, const StreamCallback&)>;
struct TurnSettings {
    std::string model;
    std::string reasoning_effort;
    Session::Mode mode  = Session::Mode::PLAN;
    ApiStandard dialect = ApiStandard::OPENAI;
    std::string connection_id;
    Route route;
};

class Controller {
public:
    Controller(std::shared_ptr<Session> session, const Config& cfg, PostFn post,
        std::function<void()> on_exit, StreamFn stream_fn = { },
        ToolRegistry tools = { }, ModelsFn models_fn = { });
    Controller(std::shared_ptr<Session> session,
        std::shared_ptr<ProviderStore> providers, PostFn post,
        std::function<void()> on_exit, StreamFn stream_fn = { },
        ToolRegistry tools = { });
    ~Controller();

    Controller(const Controller&)            = delete;
    Controller& operator=(const Controller&) = delete;

    void submit(std::string text);
    void toggle_mode();
    void set_error(std::string msg);
    void clear_error();
    void close_modal();
    void resolve_modal(ModalResult result);
    void enqueue_user_modal(ModalPayload payload);
    void cancel_queued(std::size_t id);
    void interrupt();
    size_t queue_size() const;
    const Session& session() const { return *session_; }
    const std::vector<SlashCommand>& commands() const { return commands_; }

private:
    void submit_message(std::string text);
    void run_slash(std::string_view cmd);
    void finish(std::string error);

    void _post(std::function<void()> f);
    std::string _system_prompt() const;
    void _spawn(
        std::vector<Message> history, StreamFn override, TurnSettings settings);
    void _drive(
        std::vector<Message> history, StreamFn override, TurnSettings settings);
    void _begin_connect(const ConnectResult& res);
    void _apply_pick(const ModelChoice& choice);

    void _drain_pending_asks(std::vector<Message>& history,
        std::string& reply_buffer, const std::string& assistant_text,
        ApiStandard dialect);
    void _apply_tool_result(const ToolCallRequest& req, const ModalResult& res,
        std::vector<Message>& tool_msgs);
    bool _model_reasons(const std::string& model) const;
    std::uint64_t _budget_for_effort(const std::string& effort) const;
    void _set_reasoning(
        ChatRequest& req, ApiStandard dialect, std::string_view effort);
    void _apply_question_result(
        const ModalResult& res, std::string& reply_buffer);
    void _apply_ask_result(const ToolCallRequest& req, const ModalResult& res,
        std::vector<Message>& tool_msgs);
    void _run_tool(const ToolCallRequest& req, std::vector<Message>& tool_msgs);
    void _present_front();
    void _drain_queued();

    std::shared_ptr<Session> session_;
    PostFn post_;
    std::function<void()> on_exit_;
    std::vector<SlashCommand> commands_;
    StreamFn stream_fn_;
    bool has_stream_override_ { false };
    ToolRegistry tools_;
    std::vector<ToolSpec> specs_plan_;
    std::vector<ToolSpec> specs_all_;
    std::function<void()> env_sub_;
    std::function<void()> provider_sub_;

    struct PendingModal {
        ModalPayload payload;
        std::shared_ptr<std::promise<ModalResult>> promise;
    };
    std::deque<PendingModal> queue_;
    mutable std::mutex queue_mutex_;
    std::set<std::string> allowed_tools_;

    std::vector<StreamEvent> stream_events_;
    std::atomic<bool> alive_ { true };
    std::shared_ptr<ProviderStore> providers_;
    std::optional<std::jthread> worker_;
    int retry_after_secs_ = 0;
};

std::string error_text(Status st);

} // namespace ursa
