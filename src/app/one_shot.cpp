#include "app/flows.h"

#include "runtime/main_thread_queue.h"
#include "turn/turn_runner.h"

#include <chrono>
#include <optional>
#include <string>

namespace ursa {

namespace {

    std::optional<std::string> first_tool_error(const SessionSnapshot& snapshot)
    {
        for (const ConversationItem& item : snapshot.items) {
            const auto* tool = std::get_if<ToolCall>(&item);
            if (tool == nullptr || !tool->result
                || tool->result->kind != ToolCall::Result::Kind::ERROR) {
                continue;
            }
            return tool->result->text.empty() ? "A tool failed."
                                              : tool->result->text;
        }
        return std::nullopt;
    }

    void wait_for_completion(
        const ApplicationState& state, MainThreadQueue& main_thread)
    {
        using namespace std::chrono_literals;
        while (state.session->has_pending_work()) {
            main_thread.wait_for_task(100ms);
            main_thread.drain();
        }
        main_thread.drain();
    }

} // namespace

OneShotResult run_one_shot(ApplicationState& state,
    MainThreadQueue& main_thread, const OneShotRequest& request)
{
    state.session->set_mode(request.mode == OneShotRequest::Mode::ASK
            ? Session::Mode::PLAN
            : Session::Mode::BUILD);
    submit(state, request.query);
    wait_for_completion(state, main_thread);

    OneShotResult result;
    if (const auto assistant = state.session->last_assistant()) {
        result.output = assistant->markdown;
    }
    if (state.session->interrupt_requested()) {
        result.kind  = OneShotResult::Kind::INTERRUPTED;
        result.error = "One-shot run interrupted.";
    } else if (state.runner->blocked_permission()) {
        result.kind  = OneShotResult::Kind::BLOCKED_PERMISSION;
        result.error = "One-shot run blocked by a required permission.";
    } else if (const std::string error = state.session->error();
        !error.empty()) {
        result.kind  = OneShotResult::Kind::PROVIDER_FAILURE;
        result.error = error;
    } else if (const auto error = first_tool_error(state.session->snapshot())) {
        result.kind  = OneShotResult::Kind::TOOL_FAILURE;
        result.error = *error;
    }
    return result;
}

int one_shot_exit_code(OneShotResult::Kind kind)
{
    switch (kind) {
    case OneShotResult::Kind::SUCCESS: return 0;
    case OneShotResult::Kind::PROVIDER_FAILURE: return 1;
    case OneShotResult::Kind::BLOCKED_PERMISSION: return 3;
    case OneShotResult::Kind::TOOL_FAILURE: return 4;
    case OneShotResult::Kind::INTERRUPTED: return 130;
    }
    return 1;
}

} // namespace ursa
