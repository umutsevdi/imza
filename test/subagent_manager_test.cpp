#include <doctest/doctest.h>

#include "agent/subsystems/subagent_manager.h"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

TEST_CASE("subagent manager publishes ordered lifecycle events")
{
    ursa::SubagentManager manager;
    std::vector<ursa::SubagentEvent::Kind> events;
    auto subscription
        = manager.subscribe([&](const ursa::SubagentEvent& event) {
              events.push_back(event.kind);
          });

    auto handle
        = manager.start("inspect", "model", "low", true, [](std::stop_token) {
              return ursa::SubagentResult { ursa::Status::OK, "report" };
          });

    REQUIRE(handle.completion.wait_for(std::chrono::seconds(1))
        == std::future_status::ready);
    CHECK(handle.completion.get().output == "report");
    REQUIRE(events.size() == 2);
    CHECK(events[0] == ursa::SubagentEvent::Kind::STARTED);
    CHECK(events[1] == ursa::SubagentEvent::Kind::COMPLETED);
    CHECK(manager.running_count() == 0);
}

TEST_CASE("hidden subagents are excluded from the visible running count")
{
    ursa::SubagentManager manager;
    std::promise<void> release;
    auto gate   = release.get_future().share();
    auto handle = manager.start(
        "title", "model", "off", false, [gate](std::stop_token) {
            gate.wait();
            return ursa::SubagentResult { ursa::Status::OK, "Title" };
        });

    CHECK(manager.running_count() == 0);
    CHECK(manager.running_count(false) == 1);
    release.set_value();
    handle.completion.wait();
}

TEST_CASE("subagent failures retain a typed status")
{
    ursa::SubagentManager manager;
    auto handle
        = manager.start("inspect", "model", "high", true, [](std::stop_token) {
              return ursa::SubagentResult { ursa::Status::NETWORK_ERROR,
                  "offline" };
          });

    const auto result = handle.completion.get();
    CHECK(result.status == ursa::Status::NETWORK_ERROR);
    REQUIRE(manager.tasks().size() == 1);
    CHECK(manager.tasks().front().state == ursa::SubagentTask::State::FAILED);
}

TEST_CASE("stopping subagents joins active workers")
{
    ursa::SubagentManager manager;
    auto handle = manager.start(
        "inspect", "model", "low", true, [](std::stop_token stop) {
            while (!stop.stop_requested()) {
                std::this_thread::yield();
            }
            return ursa::SubagentResult { ursa::Status::API_ERROR,
                "cancelled" };
        });

    manager.stop();

    CHECK(handle.completion.wait_for(std::chrono::seconds(1))
        == std::future_status::ready);
    CHECK(manager.running_count() == 0);
}

TEST_CASE("a single subagent can be cancelled")
{
    ursa::SubagentManager manager;
    auto handle = manager.start(
        "inspect", "model", "low", true, [](std::stop_token stop) {
            while (!stop.stop_requested()) {
                std::this_thread::yield();
            }
            return ursa::SubagentResult { ursa::Status::CANCELLED,
                "cancelled" };
        });

    CHECK(manager.cancel(handle.id));
    REQUIRE(handle.completion.wait_for(std::chrono::seconds(1))
        == std::future_status::ready);
    CHECK(handle.completion.get().status == ursa::Status::CANCELLED);
    CHECK_FALSE(manager.cancel(handle.id + 1));
}
