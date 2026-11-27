#include "app/flows.h"

#include "conversation/persistence.h"
#include "network/json.h"
#include "platform/config.h"
#include "platform/json_file.h"
#include "runtime/main_thread_queue.h"
#include "turn/turn_runner.h"

#include <chrono>
#include <optional>
#include <string>

namespace imza {

struct OutputRecord {
    std::string kind;
    std::string error;
    std::string output;
    std::string session_file;
};

namespace {

    std::optional<std::string> first_tool_error(const SessionSnapshot& snapshot)
    {
        for (const ConversationItem& item : *snapshot.items) {
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

OneShotResult run_one_shot(
    ApplicationState& state, const OneShotRequest& request)
{
    MainThreadQueue& main_thread = *state.main_thread;
    state.session->set_mode(request.mode == OneShotRequest::Mode::ASK
            ? Session::Mode::PLAN
            : Session::Mode::BUILD);
    // Skill mentions resolve at submit time, so give the asynchronous
    // workspace scan a chance to publish the catalogue first; an empty
    // catalogue after the timeout still runs, degraded but functional.
    wait_for_environment_ready(state, 5000);
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

void finish_one_shot(ApplicationState& state, const OneShotRequest& request,
    OneShotResult& result)
{
    if (save_unattended_session(*state.session) == Status::OK) {
        result.session_file
            = unattended_dir() / (state.session->session_id() + ".json");
    }
    if (!request.output_json_path) {
        return;
    }
    const char* kind = "success";
    switch (result.kind) {
    case OneShotResult::Kind::SUCCESS: kind = "success"; break;
    case OneShotResult::Kind::BLOCKED_PERMISSION:
        kind = "blocked_permission";
        break;
    case OneShotResult::Kind::INTERRUPTED: kind = "interrupted"; break;
    case OneShotResult::Kind::PROVIDER_FAILURE:
        kind = "provider_failure";
        break;
    case OneShotResult::Kind::TOOL_FAILURE: kind = "tool_failure"; break;
    }
    const OutputRecord record { kind, result.error, result.output,
        result.session_file ? utf8_from_path(*result.session_file)
                            : std::string { } };
    if (const std::optional<std::string> serialized = json_dump_checked(record);
        serialized) {
        const Status st
            = write_json_file(*request.output_json_path, *serialized + "\n");
        if (st != Status::OK && result.error.empty()) {
            result.error = "Failed to write the output record.";
        }
    } else if (result.error.empty()) {
        result.error = "Failed to serialize the output record.";
    }
}

} // namespace imza
