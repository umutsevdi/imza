#include <doctest/doctest.h>

#include "runtime/main_thread_queue.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace std::chrono_literals;

TEST_CASE("main thread queue defers tasks until drained")
{
    ursa::MainThreadQueue queue;
    int value = 0;

    queue.post([&value] { value = 1; });

    CHECK(value == 0);
    queue.drain();
    CHECK(value == 1);
}

TEST_CASE("main thread queue drains tasks in posting order")
{
    ursa::MainThreadQueue queue;
    std::string order;

    queue.post([&order] { order += 'a'; });
    queue.post([&order] { order += 'b'; });
    queue.post([&order] { order += 'c'; });
    queue.drain();

    CHECK(order == "abc");
}

TEST_CASE("main thread queue notifies subscribers of pending work")
{
    ursa::MainThreadQueue queue;
    int notifications = 0;
    queue.post([] { });

    auto subscription = queue.subscribe([&notifications] { ++notifications; });
    CHECK(notifications == 1);

    queue.drain();
    queue.post([] { });
    CHECK(notifications == 2);
}

TEST_CASE("main thread queue accepts work from another thread")
{
    ursa::MainThreadQueue queue;
    std::atomic<bool> ran = false;
    std::jthread worker([&queue, &ran] { queue.post([&ran] { ran = true; }); });
    worker.join();

    REQUIRE(queue.wait_for_task(10ms));
    CHECK_FALSE(ran.load());
    queue.drain();
    CHECK(ran.load());
}
