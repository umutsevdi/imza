#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "agent/application_state.h"
#include "agent/tools.h"
#include "common/types.h"
#include "network/network.h"
#include "subsystems/session.h"

namespace ursa {

class SkillStore;
class ProviderStore;

using SubagentToolFn
    = std::function<void(const ToolCallRequest&, std::vector<Message>&)>;

struct TurnSettings {
    std::string model;
    std::string reasoning_effort;
    Session::Mode mode  = Session::Mode::PLAN;
    ApiStandard dialect = ApiStandard::OPENAI;
    std::string connection_id;
    Route route;
};

class TurnRunner {
public:
    TurnRunner(ApplicationState& state, PostFn post, std::vector<Tool> tools,
        StreamFn stream_fn, ModalRequestFn modal_request,
        std::shared_ptr<SkillStore> skills, SubagentToolFn subagent_tool,
        std::function<void(std::string)> on_finish);
    ~TurnRunner();

    TurnRunner(const TurnRunner&)            = delete;
    TurnRunner& operator=(const TurnRunner&) = delete;

    void spawn(std::vector<Message> history, TurnSettings settings);
    void clear();
    void stop();
    void set_on_finish(std::function<void(std::string)> on_finish);
    void set_subagent_tool(SubagentToolFn subagent_tool);
    bool has_stream_override() const { return has_stream_override_; }
    const StreamFn& stream_fn() const { return stream_fn_; }

private:
    void _drive(std::vector<Message> history, TurnSettings settings);
    bool _compact_history(std::vector<Message>& history,
        const TurnSettings& settings, std::uint64_t prompt_tokens);
    void _drain_pending_asks(std::vector<Message>& history,
        std::string& reply_buffer, const std::string& assistant_text,
        ApiStandard dialect);
    void _apply_tool_result(const ToolCallRequest& req, const ModalResult& res,
        std::vector<Message>& tool_msgs);
    void _apply_ask_result(const ToolCallRequest& req, const ModalResult& res,
        std::vector<Message>& tool_msgs);
    void _apply_question_result(
        const ModalResult& res, std::string& reply_buffer);
    void _run_tool(const ToolCallRequest& req, std::vector<Message>& tool_msgs);
    void _post(std::function<void()> f);

    ApplicationState* state_;
    PostFn post_;
    ModalRequestFn modal_request_;
    std::shared_ptr<SkillStore> skills_;
    SubagentToolFn subagent_tool_;
    std::function<void(std::string)> on_finish_;
    StreamFn stream_fn_;
    bool has_stream_override_ { false };
    std::vector<Tool> tools_;
    std::vector<ToolSpec> specs_plan_;
    std::vector<ToolSpec> specs_all_;
    std::set<std::string> allowed_tools_;
    std::vector<StreamEvent> stream_events_;
    std::atomic<bool> alive_ { true };
    std::optional<std::jthread> worker_;
    int retry_after_secs_ = 0;
};

void apply_reasoning(ChatRequest& req, ApiStandard dialect,
    std::string_view effort, const ProviderStore& providers);

} // namespace ursa
