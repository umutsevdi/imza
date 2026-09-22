#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "app/application_state.h"
#include "common/types.h"
#include "conversation/session.h"
#include "network/network.h"
#include "tools/tool.h"

namespace imza {

class SkillStore;
class ProviderStore;
struct PermissionEvaluation;

struct TurnSettings {
    std::string model;
    std::string reasoning_effort;
    Session::Mode mode  = Session::Mode::PLAN;
    ApiStandard dialect = ApiStandard::OPENAI;
    std::string connection_id;
    Route route;
};

struct ProviderSelection;

TurnSettings make_turn_settings(
    const ProviderSelection& selection, Session::Mode mode);

// Bounded transcript shared by automatic compaction and sidechat seeding.
std::string conversation_transcript(
    const std::string& compacted_summary, const SessionSnapshot& snapshot);

class StreamUpdateBuffer {
public:
    StreamUpdateBuffer(PostFn post, std::shared_ptr<Session> session);
    ~StreamUpdateBuffer();

    StreamUpdateBuffer(const StreamUpdateBuffer&)            = delete;
    StreamUpdateBuffer& operator=(const StreamUpdateBuffer&) = delete;

    void push(const StreamEvent& event, const ModelPricing& pricing);
    void finish();

private:
    struct State;
    static void _publish(const std::shared_ptr<State>& state);
    std::shared_ptr<State> _state;
    std::jthread _publisher;
};

class TurnRunner final : public ApplicationComponent {
public:
    TurnRunner(ApplicationState& state, PostFn post, std::vector<Tool> tools,
        StreamFn stream_fn, ModalRequestFn modal_request,
        std::shared_ptr<SkillStore> skills,
        std::function<void(std::string)> on_finish);
    ~TurnRunner();

    TurnRunner(const TurnRunner&)            = delete;
    TurnRunner& operator=(const TurnRunner&) = delete;

    void spawn(std::vector<Message> history, TurnSettings settings);
    void clear();
    void stop();
    void set_on_finish(std::function<void(std::string)> on_finish);
    bool has_stream_override() const { return _has_stream_override; }
    const StreamFn& stream_fn() const { return _stream_fn; }
    const std::vector<ToolSpec>& specs() const { return _specs_all; }
    Status run_stream(const ChatRequest& req, const Route& route,
        const StreamCallback& cb) const;
    bool blocked_permission() const { return _blocked_permission.load(); }

private:
    void _drive(std::vector<Message> history, TurnSettings settings);
    bool _compact_history(std::vector<Message>& history,
        const TurnSettings& settings, std::uint64_t prompt_tokens);
    void _drain_pending_asks(std::vector<Message>& history,
        std::string& reply_buffer, const std::string& assistant_text,
        ApiStandard dialect, Session::Mode mode);
    void _apply_tool_result(const PermissionEvaluation& evaluation,
        const ModalResult& res, Session::Mode mode,
        std::vector<Message>& tool_msgs);
    void _apply_question_result(
        const ModalResult& res, std::string& reply_buffer);
    void _reject_tool(const ToolCallRequest& req, std::string reason,
        std::vector<Message>& tool_msgs);
    void _finish_tool(const ToolCallRequest& req, ToolCall::Result::Kind kind,
        const std::string& history_text, std::vector<Message>& tool_msgs);
    void _finish_tool(const ToolCallRequest& req, ToolCall::Result::Kind kind,
        std::string result_text, std::string history_text,
        std::vector<Message>& tool_msgs);
    void _run_tool(const PermissionEvaluation& evaluation, Session::Mode mode,
        std::vector<Message>& tool_msgs);
    void _post(std::function<void()> f);

    ApplicationState* _state;
    PostFn _post_fn;
    ModalRequestFn _modal_request;
    std::shared_ptr<SkillStore> _skills;
    std::function<void(std::string)> _on_finish;
    StreamFn _stream_fn;
    bool _has_stream_override { false };
    std::vector<Tool> _tools;
    std::vector<ToolSpec> _specs_all;
    std::vector<StreamEvent> _stream_events;
    std::atomic<bool> _alive { true };
    std::atomic<bool> _blocked_permission { false };
    std::optional<std::jthread> _worker;
    int _retry_after_secs = 0;
};

void apply_reasoning(ChatRequest& req, ApiStandard dialect,
    std::string_view effort, const ProviderStore& providers);

} // namespace imza
