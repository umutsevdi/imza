#include "ui/frame_pulse.h"

namespace imza {

FramePulse::FramePulse(std::chrono::milliseconds interval,
    std::function<bool()> awake, std::function<void()> wake)
    : _interval(interval)
    , _awake(std::move(awake))
    , _wake(std::move(wake))
    , _worker([this](std::stop_token stop) { _run(stop); })
{
}

FramePulse::~FramePulse() { stop(); }

void FramePulse::stop()
{
    if (_worker.joinable()) {
        _worker.request_stop();
        _worker.join();
    }
}

bool FramePulse::_sleep(std::stop_token stop, std::chrono::milliseconds delay)
{
    // Waits in slices: stop() joins from the render thread, so an
    // uninterruptible sleep would stall the frame loop it serves.
    const auto slice    = std::chrono::milliseconds(20);
    const auto deadline = std::chrono::steady_clock::now() + delay;
    for (;;) {
        if (stop.stop_requested()) {
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return true;
        }
        std::this_thread::sleep_for(
            std::min<std::chrono::steady_clock::duration>(
                slice, deadline - now));
    }
}

void FramePulse::_run(std::stop_token stop)
{
    if (!_awake || !_wake) {
        return;
    }
    // Transition latency does not depend on this cadence: state-change
    // signals request frames directly, so a slow idle poll delays nothing.
    const auto idle = std::max<std::chrono::milliseconds>(
        2 * _interval, std::chrono::milliseconds(150));
    for (;;) {
        if (stop.stop_requested()) {
            return;
        }
        if (!_awake()) {
            if (!_sleep(stop, idle)) {
                return;
            }
            continue;
        }
        _wake();
        if (!_sleep(stop, _interval)) {
            return;
        }
    }
}

} // namespace imza
