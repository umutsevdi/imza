#include <doctest/doctest.h>

#include "runtime/main_thread_queue.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace std::chrono_literals;

TEST_CASE("main thread queue defers tasks until drained in posting order")
{
    imza::MainThreadQueue queue;
    std::string order;

    queue.post([&order] { order += 'a'; });
    queue.post([&order] { order += 'b'; });
    queue.post([&order] { order += 'c'; });

    CHECK(order.empty());
    queue.drain();
    CHECK(order == "abc");
}

TEST_CASE("main thread queue notifies subscribers of pending work")
{
    imza::MainThreadQueue queue;
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
    imza::MainThreadQueue queue;
    std::atomic<bool> ran = false;
    std::jthread worker([&queue, &ran] { queue.post([&ran] { ran = true; }); });
    worker.join();

    REQUIRE(queue.wait_for_task(10ms));
    CHECK_FALSE(ran.load());
    queue.drain();
    CHECK(ran.load());
}
