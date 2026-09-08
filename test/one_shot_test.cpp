#include "app/application_state.h"
#include "app/flows.h"
#include "runtime/main_thread_queue.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace {

imza::Config one_shot_config()
{
    imza::Config config;
    imza::Connection connection;
    connection.id          = "test";
    connection.provider_id = "test";
    config.providers.push_back(connection);
    config.last_used = imza::LastUsed { "test", "model" };
    return config;
}

std::shared_ptr<imza::ApplicationState> make_one_shot_state(
    imza::MainThreadQueue& queue, imza::StreamFn stream,
    imza::RuntimeFlag flags          = imza::WEB,
    std::atomic<std::size_t>* posted = nullptr)
{
    return imza::make_application_state(
        [&queue, posted](std::function<void()> task) {
            if (posted != nullptr) {
                posted->fetch_add(1);
            }
            queue.post(std::move(task));
        },
        one_shot_config(), std::move(stream), flags);
}

} // namespace

TEST_CASE("one-shot ask runs in Plan mode and returns assistant output")
{
    imza::MainThreadQueue queue;
    auto state = make_one_shot_state(queue,
        [](const imza::ChatRequest&, const imza::StreamCallback& callback) {
            callback(imza::make_connected_event());
            callback(imza::make_delta_event("summary"));
            callback(imza::make_done_event());
            return imza::Status::OK;
        });

    const imza::OneShotResult result = imza::run_one_shot(
        *state, queue, { imza::OneShotRequest::Mode::ASK, "summarize" });

    CHECK(state->session->mode() == imza::Session::Mode::PLAN);
    CHECK(result.kind == imza::OneShotResult::Kind::SUCCESS);
    CHECK(result.output == "summary");
    CHECK(imza::one_shot_exit_code(result.kind) == 0);
}

TEST_CASE("one-shot exec runs in Build mode")
{
    imza::MainThreadQueue queue;
    auto state = make_one_shot_state(queue,
        [](const imza::ChatRequest&, const imza::StreamCallback& callback) {
            callback(imza::make_delta_event("built"));
            callback(imza::make_done_event());
            return imza::Status::OK;
        });

    const auto result = imza::run_one_shot(
        *state, queue, { imza::OneShotRequest::Mode::EXEC, "build" });

    CHECK(state->session->mode() == imza::Session::Mode::BUILD);
    CHECK(result.kind == imza::OneShotResult::Kind::SUCCESS);
    CHECK(result.output == "built");
}

TEST_CASE("one-shot coalesces adjacent streaming deltas")
{
    imza::MainThreadQueue queue;
    std::atomic<std::size_t> posted = 0;
    auto state                      = make_one_shot_state(
        queue,
        [](const imza::ChatRequest&, const imza::StreamCallback& callback) {
            for (std::size_t index = 0; index < 1000; ++index) {
                callback(imza::make_delta_event("x"));
            }
            callback(imza::make_done_event());
            return imza::Status::OK;
        },
        imza::WEB, &posted);
    while (!state->environment->ready()) {
        std::this_thread::sleep_for(std::chrono::milliseconds { 1 });
    }
    queue.drain();
    posted.store(0);

    const auto result = imza::run_one_shot(
        *state, queue, { imza::OneShotRequest::Mode::ASK, "stream" });

    CHECK(result.output == std::string(1000, 'x'));
    CHECK(posted.load() < 50);
}

TEST_CASE("one-shot preserves coalesced reasoning and its signature")
{
    imza::MainThreadQueue queue;
    auto state = make_one_shot_state(queue,
        [](const imza::ChatRequest&, const imza::StreamCallback& callback) {
            callback(imza::make_reasoning_event("first "));
            callback(imza::make_reasoning_event("second", "signature"));
            callback(imza::make_delta_event("answer"));
            callback(imza::make_done_event());
            return imza::Status::OK;
        });

    const auto result = imza::run_one_shot(
        *state, queue, { imza::OneShotRequest::Mode::ASK, "reason" });
    const auto assistant = state->session->last_assistant();

    REQUIRE(assistant.has_value());
    CHECK(result.output == "answer");
    CHECK(assistant->reasoning == "first second");
    CHECK(assistant->reasoning_signature == "signature");
}

TEST_CASE("one-shot reports unattended permission blocks without a modal")
{
    imza::MainThreadQueue queue;
    auto state = make_one_shot_state(
        queue,
        [](const imza::ChatRequest& request,
            const imza::StreamCallback& callback) {
            if (request.messages.back().type == imza::Message::Type::USER) {
                callback(imza::make_tool_call_event({ "shell",
                    R"({"command":"custom blocked"})", "", "call" }));
            } else {
                callback(imza::make_delta_event("permission required"));
            }
            callback(imza::make_done_event());
            return imza::Status::OK;
        },
        static_cast<imza::RuntimeFlag>(imza::WEB | imza::SHELL));

    const auto result = imza::run_one_shot(
        *state, queue, { imza::OneShotRequest::Mode::EXEC, "run it" });

    CHECK(result.kind == imza::OneShotResult::Kind::BLOCKED_PERMISSION);
    CHECK(imza::one_shot_exit_code(result.kind) == 3);
    CHECK(state->queue.size() == 0);
    CHECK(state->session->modal().index() == 0);
}

TEST_CASE("one-shot reports provider failures")
{
    imza::MainThreadQueue queue;
    auto state = make_one_shot_state(queue,
        [](const imza::ChatRequest&, const imza::StreamCallback& callback) {
            callback(imza::make_error_event(
                imza::Status::BUDGET_EXCEEDED, "no credits"));
            return imza::Status::BUDGET_EXCEEDED;
        });

    const auto result = imza::run_one_shot(
        *state, queue, { imza::OneShotRequest::Mode::ASK, "summarize" });

    CHECK(result.kind == imza::OneShotResult::Kind::PROVIDER_FAILURE);
    CHECK(result.error.find("no credits") != std::string::npos);
    CHECK(imza::one_shot_exit_code(result.kind) == 1);
}
