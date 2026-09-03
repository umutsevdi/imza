#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "agent/application_state.h"
#include "subsystems/session.h"
#include "subsystems/subagent_manager.h"
#include "agent/turn_runner.h"

namespace ursa {

struct ProviderSelection;

struct SubagentOptions {
    bool visible = true;
    std::chrono::seconds timeout { 0 };
    std::optional<std::uint64_t> max_output_tokens;
    std::shared_ptr<Session> transcript;
};

class DelegationRunner {
public:
    DelegationRunner(ApplicationState& state, PostFn post,
        ModalRequestFn modal_request, TurnRunner& runner);

    DelegationRunner(const DelegationRunner&)            = delete;
    DelegationRunner& operator=(const DelegationRunner&) = delete;

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
