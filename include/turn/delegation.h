#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "conversation/session.h"
#include "runtime/callbacks.h"
#include "runtime/subagent_subsystem.h"
#include "turn/turn_runner.h"

namespace imza {

struct ApplicationState;
struct ProviderSelection;

struct SubagentOptions {
    bool visible = true;
    // Send only the user message, with no system prompt: one-shot passes
    // whose instructions are self-contained.
    bool bare_system = false;
    std::chrono::seconds timeout { 0 };
    std::optional<std::uint64_t> max_output_tokens;
    std::shared_ptr<Session> transcript;
};

class Delegation final : public Subsystem {
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
    ToolOutput run_subagents(const ToolCallRequest& req);
    void spawn_title(std::string input, TurnSettings settings);
    SubagentChat subagent_chat(std::size_t id, std::string title) const;
    SubagentChat subagent_chat(const ToolCall& call, std::size_t index) const;

private:
    ApplicationState* _state;
    PostFn _post;
    ModalRequestFn _modal_request;
    TurnRunner& _runner;
};

} // namespace imza
