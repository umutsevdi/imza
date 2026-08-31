#include <doctest/doctest.h>

#include "ursa_signal.h"

TEST_CASE("signal publishes to active subscriptions")
{
    ursa::Signal<int> signal;
    int total   = 0;
    auto first  = signal.subscribe([&](int value) { total += value; });
    auto second = signal.subscribe([&](int value) { total += value * 2; });

    signal.publish(3);

    CHECK(total == 9);
}

TEST_CASE("signal subscription disconnects on destruction")
{
    ursa::Signal<> signal;
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
    ursa::Signal<> signal;
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
    ursa::Signal<> signal;
    int calls = 0;
    ursa::Signal<>::Subscription subscription;
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
    ursa::Signal<>::Subscription subscription;
    {
        ursa::Signal<> signal;
        subscription = signal.subscribe([] { });
    }

    subscription.disconnect();
}
