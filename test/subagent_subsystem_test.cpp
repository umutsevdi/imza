#include <doctest/doctest.h>

#include "runtime/subagent_subsystem.h"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

// Worker that spins until cancelled, reporting `status`.
imza::SubagentResult spin_until_stopped(
    std::stop_token stop, imza::Status status = imza::Status::CANCELLED)
{
    while (!stop.stop_requested()) {
        std::this_thread::yield();
    }
    return imza::SubagentResult { status, "cancelled" };
}

TEST_CASE("subagent manager publishes ordered lifecycle events")
{
    imza::SubagentSubsystem manager;
    std::vector<imza::SubagentEvent::Kind> events;
    auto subscription
        = manager.subscribe_to_change([&](const imza::SubagentEvent& event) {
              events.push_back(event.kind);
          });

    auto handle
        = manager.start("inspect", "model", "low", true, [](std::stop_token) {
              return imza::SubagentResult { imza::Status::OK, "report" };
          });

    REQUIRE(handle.completion.wait_for(std::chrono::seconds(1))
        == std::future_status::ready);
    CHECK(handle.completion.get().output == "report");
    REQUIRE(events.size() == 2);
    CHECK(events[0] == imza::SubagentEvent::Kind::STARTED);
    CHECK(events[1] == imza::SubagentEvent::Kind::COMPLETED);
    CHECK(manager.running_count() == 0);
}

TEST_CASE("hidden subagents are excluded from the visible running count")
{
    imza::SubagentSubsystem manager;
    std::promise<void> release;
    auto gate   = release.get_future().share();
    auto handle = manager.start(
        "title", "model", "off", false, [gate](std::stop_token) {
            gate.wait();
            return imza::SubagentResult { imza::Status::OK, "Title" };
        });

    CHECK(manager.running_count() == 0);
    CHECK(manager.running_count(false) == 1);
    release.set_value();
    handle.completion.wait();
}

TEST_CASE("subagent failures retain a typed status")
{
    imza::SubagentSubsystem manager;
    auto handle
        = manager.start("inspect", "model", "high", true, [](std::stop_token) {
              return imza::SubagentResult { imza::Status::NETWORK_ERROR,
                  "offline" };
          });

    const auto result = handle.completion.get();
    CHECK(result.status == imza::Status::NETWORK_ERROR);
    REQUIRE(manager.tasks().size() == 1);
    CHECK(manager.tasks().front().state == imza::SubagentTask::State::FAILED);
}

TEST_CASE("completed subagents can release retained task state")
{
    imza::SubagentSubsystem manager;
    auto handle = manager.start(
        "large prompt", "model", "low", true, [](std::stop_token) {
            return imza::SubagentResult { imza::Status::OK, "large output" };
        });

    REQUIRE(handle.completion.wait_for(std::chrono::seconds(1))
        == std::future_status::ready);
    manager.prune_completed();

    CHECK(manager.tasks().empty());
    CHECK_FALSE(manager.cancel(handle.id));
    CHECK(handle.completion.get().output == "large output");
}

TEST_CASE("stopping subagents joins active workers")
{
    imza::SubagentSubsystem manager;
    auto handle = manager.start("inspect", "model", "low", true,
        [](std::stop_token stop) { return spin_until_stopped(stop); });

    manager.stop();

    CHECK(handle.completion.wait_for(std::chrono::seconds(1))
        == std::future_status::ready);
    CHECK(manager.running_count() == 0);
}

TEST_CASE("a single subagent can be cancelled")
{
    imza::SubagentSubsystem manager;
    auto handle = manager.start("inspect", "model", "low", true,
        [](std::stop_token stop) { return spin_until_stopped(stop); });

    CHECK(manager.cancel(handle.id));
    REQUIRE(handle.completion.wait_for(std::chrono::seconds(1))
        == std::future_status::ready);
    CHECK(handle.completion.get().status == imza::Status::CANCELLED);
    CHECK_FALSE(manager.cancel(handle.id + 1));
}

// A completion callback runs on the finishing subagent's worker; pruning
// there must not reap (and join) that same thread.
TEST_CASE("pruning from a completion callback does not join the caller")
{
    imza::SubagentSubsystem manager;
    std::promise<void> pruned;
    std::atomic<bool> callback_ran { false };
    auto handle = manager.start(
        "inspect", "model", "low", true,
        [](std::stop_token) {
            return imza::SubagentResult { imza::Status::OK, "report" };
        },
        [&](const imza::SubagentResult&) {
            manager.prune_completed();
            callback_ran.store(true);
            pruned.set_value();
        });

    CHECK(pruned.get_future().wait_for(std::chrono::seconds(3))
        == std::future_status::ready);
    CHECK(callback_ran.load());
    // The caller's own worker survives the in-callback prune and is reaped
    // by a later prune from this thread.
    manager.prune_completed();
    CHECK(manager.tasks().empty());
    CHECK(handle.completion.get().output == "report");
}

TEST_CASE("task snapshots a single subagent by id")
{
    imza::SubagentSubsystem manager;
    auto handle
        = manager.start("inspect", "model", "low", true, [](std::stop_token) {
              return imza::SubagentResult { imza::Status::OK, "report" };
          });
    CHECK(handle.completion.wait_for(std::chrono::seconds(3))
        == std::future_status::ready);

    const auto task = manager.task(handle.id);
    REQUIRE(task != nullptr);
    CHECK(task->id == handle.id);
    CHECK(task->prompt == "inspect");
    CHECK(manager.task(handle.id + 1000) == nullptr);
}
