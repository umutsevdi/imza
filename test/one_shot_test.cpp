#include "app/application_state.h"
#include "app/flows.h"
#include "network/json.h"
#include "runtime/main_thread_queue.h"
#include "test_fs.h"
#include "test_state.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace {

// The state owns its main-thread queue; an empty PostFn routes into it,
// so run_one_shot drains the same queue the workers post to. `posted`
// counts everything posted through the state's PostFn.
std::shared_ptr<imza::ApplicationState> make_one_shot_state(
    imza::StreamFn stream, imza::RuntimeFlag flags = imza::WEB,
    std::atomic<std::size_t>* posted = nullptr)
{
    imza::PostFn post;
    if (posted != nullptr) {
        post = [posted](std::function<void()> task) {
            posted->fetch_add(1);
            task();
        };
    }
    return imza::test::make_test_state(
        std::move(post), imza::test::test_config(), std::move(stream), flags);
}

} // namespace

TEST_CASE("one-shot ask runs in Plan mode and returns assistant output")
{
    std::atomic_bool received_plan_mode = false;
    auto state                          = make_one_shot_state(
        [&received_plan_mode](const imza::ChatRequest& request,
            const imza::StreamCallback& callback) {
            received_plan_mode.store(request.messages.front().content.find(
                                         "<runtime-mode name=\"plan\"/>")
                != std::string::npos);
            callback(imza::make_connected_event());
            callback(imza::make_delta_event("summary"));
            callback(imza::make_done_event());
            return imza::Status::OK;
        });

    const imza::OneShotResult result = imza::run_one_shot(
        *state, { imza::OneShotRequest::Mode::ASK, "summarize" });

    CHECK(state->session->mode() == imza::Session::Mode::PLAN);
    CHECK(received_plan_mode.load());
    CHECK(result.kind == imza::OneShotResult::Kind::SUCCESS);
    CHECK(result.output == "summary");
    CHECK(imza::one_shot_exit_code(result.kind) == 0);
}

TEST_CASE("one-shot exec runs in Build mode")
{
    std::atomic_bool received_build_mode = false;
    auto state                           = make_one_shot_state(
        [&received_build_mode](const imza::ChatRequest& request,
            const imza::StreamCallback& callback) {
            received_build_mode.store(request.messages.front().content.find(
                                          "<runtime-mode name=\"build\"/>")
                != std::string::npos);
            callback(imza::make_delta_event("built"));
            callback(imza::make_done_event());
            return imza::Status::OK;
        });

    const auto result = imza::run_one_shot(
        *state, { imza::OneShotRequest::Mode::EXEC, "build" });

    CHECK(state->session->mode() == imza::Session::Mode::BUILD);
    CHECK(received_build_mode.load());
    CHECK(result.kind == imza::OneShotResult::Kind::SUCCESS);
    CHECK(result.output == "built");
}

TEST_CASE("one-shot coalesces adjacent streaming deltas")
{
    std::atomic<std::size_t> posted = 0;
    auto state                      = make_one_shot_state(
        [](const imza::ChatRequest&, const imza::StreamCallback& callback) {
            for (std::size_t index = 0; index < 1000; ++index) {
                callback(imza::make_delta_event("x"));
            }
            callback(imza::make_done_event());
            return imza::Status::OK;
        },
        imza::WEB, &posted);
    REQUIRE(
        imza::test::wait_until([&] { return state->environment->ready(); }));
    posted.store(0);

    const auto result = imza::run_one_shot(
        *state, { imza::OneShotRequest::Mode::ASK, "stream" });

    CHECK(result.output == std::string(1000, 'x'));
    CHECK(posted.load() < 50);
}

TEST_CASE("one-shot preserves coalesced reasoning and its signature")
{
    auto state = make_one_shot_state(
        [](const imza::ChatRequest&, const imza::StreamCallback& callback) {
            callback(imza::make_reasoning_event("first "));
            callback(imza::make_reasoning_event("second", "signature"));
            callback(imza::make_delta_event("answer"));
            callback(imza::make_done_event());
            return imza::Status::OK;
        });

    const auto result = imza::run_one_shot(
        *state, { imza::OneShotRequest::Mode::ASK, "reason" });
    const auto assistant = state->session->last_assistant();

    REQUIRE(assistant.has_value());
    CHECK(result.output == "answer");
    CHECK(assistant->reasoning == "first second");
    CHECK(assistant->reasoning_signature == "signature");
}

TEST_CASE("one-shot reports unattended permission blocks without a modal")
{
    // Outside the workspace and the trusted temporary root, so the write
    // gate raises ASK; without ATTENDED the run must fail closed.
    const imza::test::TempDir scratch;
    const std::filesystem::path path = scratch.path / "imza-one-shot-blocked";
    auto state                       = make_one_shot_state(
        [path](const imza::ChatRequest& request,
            const imza::StreamCallback& callback) {
            if (request.messages.back().type == imza::Message::Type::USER) {
                imza::JsonValue arguments = imza::JsonValue::object_t { };
                arguments.get<imza::JsonValue::object_t>()["script"]
                    = imza::JsonValue("assert(not imza.fs.write([["
                        + path.string() + "]], 'no'))");
                callback(imza::make_tool_call_event(
                    { "lua", imza::json_dump(arguments), "", "call" }));
            } else {
                callback(imza::make_delta_event("permission required"));
            }
            callback(imza::make_done_event());
            return imza::Status::OK;
        },
        static_cast<imza::RuntimeFlag>(imza::WEB | imza::SHELL));

    const auto result = imza::run_one_shot(
        *state, { imza::OneShotRequest::Mode::EXEC, "run it" });

    CHECK(result.kind == imza::OneShotResult::Kind::BLOCKED_PERMISSION);
    CHECK(imza::one_shot_exit_code(result.kind) == 3);
    CHECK(state->session->queue.size() == 0);
    CHECK(state->session->modal().index() == 0);
    CHECK_FALSE(std::filesystem::exists(path));
}

TEST_CASE("one-shot reports provider failures")
{
    auto state = make_one_shot_state(
        [](const imza::ChatRequest&, const imza::StreamCallback& callback) {
            callback(imza::make_error_event(
                imza::Status::BUDGET_EXCEEDED, "no credits"));
            return imza::Status::BUDGET_EXCEEDED;
        });

    const auto result = imza::run_one_shot(
        *state, { imza::OneShotRequest::Mode::ASK, "summarize" });

    CHECK(result.kind == imza::OneShotResult::Kind::PROVIDER_FAILURE);
    CHECK(result.error.find("no credits") != std::string::npos);
    CHECK(imza::one_shot_exit_code(result.kind) == 1);
}

TEST_CASE("one-shot retries transient server errors before stream data")
{
    std::atomic<int> calls = 0;
    auto state = make_one_shot_state([&calls](const imza::ChatRequest&,
                                         const imza::StreamCallback& callback) {
        if (calls.fetch_add(1) == 0) {
            callback(imza::make_error_event(
                imza::Status::SERVER_ERROR, "upstream unavailable"));
            return imza::Status::SERVER_ERROR;
        }
        callback(imza::make_delta_event("recovered"));
        callback(imza::make_done_event());
        return imza::Status::OK;
    });

    const auto result
        = imza::run_one_shot(*state, { imza::OneShotRequest::Mode::ASK, "go" });

    CHECK(result.kind == imza::OneShotResult::Kind::SUCCESS);
    CHECK(result.output == "recovered");
    CHECK(calls.load() == 2);
}
