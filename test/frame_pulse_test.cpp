#include "ui/frame_pulse.h"

#include <atomic>
#include <chrono>
#include <thread>

#include <doctest/doctest.h>

namespace imza {

TEST_CASE("frame pulse wakes while its predicate holds")
{
    std::atomic<int> wakes { 0 };
    std::atomic<bool> awake { true };

    FramePulse pulse(
        std::chrono::milliseconds(10), [&awake] { return awake.load(); },
        [&wakes] { ++wakes; });

    const auto deadline
        = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
    while (wakes.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(wakes.load() > 0);

    SUBCASE("goes quiet when the predicate clears, resumes on demand")
    {
        awake.store(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const int settled = wakes.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(320));
        CHECK(wakes.load() == settled);

        awake.store(true);
        const auto resumed
            = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
        while (wakes.load() == settled
            && std::chrono::steady_clock::now() < resumed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK(wakes.load() > settled);
    }
}

} // namespace imza
