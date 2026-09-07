#include "app/application_state.h"
#include "app/flows.h"
#include "runtime/main_thread_queue.h"

#include <doctest/doctest.h>

#include <functional>
#include <memory>
#include <string>

namespace {

ursa::Config one_shot_config()
{
    ursa::Config config;
    ursa::Connection connection;
    connection.id          = "test";
    connection.provider_id = "test";
    config.providers.push_back(connection);
    config.last_used = ursa::LastUsed { "test", "model" };
    return config;
}

std::shared_ptr<ursa::ApplicationState> make_one_shot_state(
    ursa::MainThreadQueue& queue, ursa::StreamFn stream,
    ursa::RuntimeFlag flags = ursa::WEB)
{
    return ursa::make_application_state(
        [&queue](std::function<void()> task) { queue.post(std::move(task)); },
        one_shot_config(), std::move(stream), flags);
}

} // namespace

TEST_CASE("one-shot ask runs in Plan mode and returns assistant output")
{
    ursa::MainThreadQueue queue;
    auto state = make_one_shot_state(queue,
        [](const ursa::ChatRequest&, const ursa::StreamCallback& callback) {
            callback(ursa::make_connected_event());
            callback(ursa::make_delta_event("summary"));
            callback(ursa::make_done_event());
            return ursa::Status::OK;
        });

    const ursa::OneShotResult result = ursa::run_one_shot(
        *state, queue, { ursa::OneShotRequest::Mode::ASK, "summarize" });

    CHECK(state->session->mode() == ursa::Session::Mode::PLAN);
    CHECK(result.kind == ursa::OneShotResult::Kind::SUCCESS);
    CHECK(result.output == "summary");
    CHECK(ursa::one_shot_exit_code(result.kind) == 0);
}

TEST_CASE("one-shot exec runs in Build mode")
{
    ursa::MainThreadQueue queue;
    auto state = make_one_shot_state(queue,
        [](const ursa::ChatRequest&, const ursa::StreamCallback& callback) {
            callback(ursa::make_delta_event("built"));
            callback(ursa::make_done_event());
            return ursa::Status::OK;
        });

    const auto result = ursa::run_one_shot(
        *state, queue, { ursa::OneShotRequest::Mode::EXEC, "build" });

    CHECK(state->session->mode() == ursa::Session::Mode::BUILD);
    CHECK(result.kind == ursa::OneShotResult::Kind::SUCCESS);
    CHECK(result.output == "built");
}

TEST_CASE("one-shot reports unattended permission blocks without a modal")
{
    ursa::MainThreadQueue queue;
    auto state = make_one_shot_state(
        queue,
        [](const ursa::ChatRequest& request,
            const ursa::StreamCallback& callback) {
            if (request.messages.back().type == ursa::Message::Type::USER) {
                callback(ursa::make_tool_call_event({ "shell",
                    R"({"command":"custom blocked"})", "", "call" }));
            } else {
                callback(ursa::make_delta_event("permission required"));
            }
            callback(ursa::make_done_event());
            return ursa::Status::OK;
        },
        static_cast<ursa::RuntimeFlag>(ursa::WEB | ursa::SHELL));

    const auto result = ursa::run_one_shot(
        *state, queue, { ursa::OneShotRequest::Mode::EXEC, "run it" });

    CHECK(result.kind == ursa::OneShotResult::Kind::BLOCKED_PERMISSION);
    CHECK(ursa::one_shot_exit_code(result.kind) == 3);
    CHECK(state->queue.size() == 0);
    CHECK(state->session->modal().index() == 0);
}

TEST_CASE("one-shot reports provider failures")
{
    ursa::MainThreadQueue queue;
    auto state = make_one_shot_state(queue,
        [](const ursa::ChatRequest&, const ursa::StreamCallback& callback) {
            callback(ursa::make_error_event(
                ursa::Status::BUDGET_EXCEEDED, "no credits"));
            return ursa::Status::BUDGET_EXCEEDED;
        });

    const auto result = ursa::run_one_shot(
        *state, queue, { ursa::OneShotRequest::Mode::ASK, "summarize" });

    CHECK(result.kind == ursa::OneShotResult::Kind::PROVIDER_FAILURE);
    CHECK(result.error.find("no credits") != std::string::npos);
    CHECK(ursa::one_shot_exit_code(result.kind) == 1);
}
