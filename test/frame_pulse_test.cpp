#include "ui/frame_pulse.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

#include <doctest/doctest.h>

namespace imza {

namespace {

// Polls until `predicate` holds or the deadline passes, so a test never
// depends on a fixed sleep being long enough on a loaded machine.
bool wait_until(const std::function<bool()>& predicate,
    std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

} // namespace

TEST_CASE("frame pulse wakes while its predicate holds")
{
    std::atomic<int> wakes { 0 };
    std::atomic<bool> awake { true };

    FramePulse pulse(
        std::chrono::milliseconds(10), [&awake] { return awake.load(); },
        [&wakes] { ++wakes; });

    REQUIRE(wait_until([&wakes] { return wakes.load() > 0; },
        std::chrono::seconds(10)));

    SUBCASE("goes quiet when the predicate clears, resumes on demand")
    {
        awake.store(false);
        // Settle: the count must stop advancing on its own. The idle poll is
        // far longer than the wake interval, so a count unchanged across
        // several idle periods means the pulse really parked.
        int last       = wakes.load();
        auto unchanged = std::chrono::steady_clock::now();
        const bool quiet = wait_until(
            [&] {
                const int now = wakes.load();
                if (now != last) {
                    last      = now;
                    unchanged = std::chrono::steady_clock::now();
                    return false;
                }
                return std::chrono::steady_clock::now() - unchanged
                    > std::chrono::milliseconds(600);
            },
            std::chrono::seconds(10));
        REQUIRE(quiet);
        const int parked = wakes.load();

        awake.store(true);
        CHECK(wait_until([&wakes, parked] { return wakes.load() > parked; },
            std::chrono::seconds(10)));
    }
}

} // namespace imza
