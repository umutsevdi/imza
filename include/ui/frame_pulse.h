#pragma once

#include <chrono>
#include <functional>
#include <thread>

namespace imza {

// Wakes the screen's animation loop at a fixed interval while `awake`
// holds. A request-driven animation loop cannot poll without redrawing, so
// spinner phases need the wake to come from outside the components.
// Screen-scoped: the pulse exists only where a terminal loop runs, so it is
// a plain object owned by the loop's scope, not an application subsystem.
// The wake runs on this worker thread and uses the same
// animation::RequestAnimationFrame call as UI signal subscribers.
class FramePulse final {
public:
    FramePulse(std::chrono::milliseconds interval, std::function<bool()> awake,
        std::function<void()> wake);
    ~FramePulse();

    FramePulse(const FramePulse&)            = delete;
    FramePulse& operator=(const FramePulse&) = delete;

    void stop();

private:
    // False when stop was requested while waiting.
    bool _sleep(std::stop_token stop, std::chrono::milliseconds delay);
    void _run(std::stop_token stop);

    std::chrono::milliseconds _interval;
    std::function<bool()> _awake;
    std::function<void()> _wake;
    std::jthread _worker;
};

} // namespace imza
