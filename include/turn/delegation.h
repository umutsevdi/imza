#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "app/application_state.h"
#include "conversation/session.h"
#include "runtime/subagent_manager.h"
#include "turn/turn_runner.h"

namespace ursa {

struct ProviderSelection;

struct SubagentOptions {
    bool visible = true;
    std::chrono::seconds timeout { 0 };
    std::optional<std::uint64_t> max_output_tokens;
    std::shared_ptr<Session> transcript;
};

class Delegation final {
public:
    Delegation(ApplicationState& state, PostFn post,
        ModalRequestFn modal_request, TurnRunner& runner);

    Delegation(const Delegation&)            = delete;
    Delegation& operator=(const Delegation&) = delete;

    SubagentHandle run_subagent(std::string prompt, std::string model,
        std::string variant, SubagentOptions options = { },
        SubagentCompleteFn complete = { });
    void submit_delegated(std::string text, const ProviderSelection& selection,
        Session::Mode mode);
    void run_subagents(
        const ToolCallRequest& req, std::vector<Message>& tool_msgs);
    void spawn_title(std::string input, TurnSettings settings);
    SubagentChat subagent_chat(std::size_t id, std::string title) const;
    SubagentChat subagent_chat(const ToolCall& call, std::size_t index) const;

private:
    ApplicationState* state_;
    PostFn post_;
    ModalRequestFn modal_request_;
    TurnRunner& runner_;
};

} // namespace ursa
