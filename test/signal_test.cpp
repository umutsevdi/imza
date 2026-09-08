#include <doctest/doctest.h>

#include "common/imza_signal.h"

TEST_CASE("signal publishes to active subscriptions")
{
    imza::Signal<int> signal;
    int total   = 0;
    auto first  = signal.subscribe([&](int value) { total += value; });
    auto second = signal.subscribe([&](int value) { total += value * 2; });

    signal.publish(3);

    CHECK(total == 9);
}

TEST_CASE("signal subscription disconnects on destruction")
{
    imza::Signal<> signal;
    int calls = 0;
    {
        auto subscription = signal.subscribe([&] { ++calls; });
        signal.publish();
    }

    signal.publish();

    CHECK(calls == 1);
}

TEST_CASE("signal subscriptions are move-only ownership tokens")
{
    imza::Signal<> signal;
    int calls   = 0;
    auto first  = signal.subscribe([&] { ++calls; });
    auto second = std::move(first);

    signal.publish();
    second.disconnect();
    signal.publish();

    CHECK(calls == 1);
}

TEST_CASE("signal permits subscription changes while publishing")
{
    imza::Signal<> signal;
    int calls = 0;
    imza::Signal<>::Subscription subscription;
    subscription = signal.subscribe([&] {
        ++calls;
        subscription.disconnect();
    });

    signal.publish();
    signal.publish();

    CHECK(calls == 1);
}

TEST_CASE("signal subscription may outlive its publisher")
{
    imza::Signal<>::Subscription subscription;
    {
        imza::Signal<> signal;
        subscription = signal.subscribe([] { });
    }

    subscription.disconnect();
}
