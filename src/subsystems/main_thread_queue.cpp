#include "subsystems/main_thread_queue.h"

#include <utility>

namespace ursa {

void MainThreadQueue::post(Task task)
{
    {
        std::lock_guard lock(_mutex);
        _tasks.push_back(std::move(task));
    }
    _ready.notify_one();
    _posted.publish();
}

void MainThreadQueue::drain()
{
    for (;;) {
        Task task;
        {
            std::lock_guard lock(_mutex);
            if (_tasks.empty()) {
                return;
            }
            task = std::move(_tasks.front());
            _tasks.pop_front();
        }
        task();
    }
}

bool MainThreadQueue::wait_for_task(std::chrono::milliseconds timeout)
{
    std::unique_lock lock(_mutex);
    return _ready.wait_for(lock, timeout, [this] { return !_tasks.empty(); });
}

bool MainThreadQueue::empty() const
{
    std::lock_guard lock(_mutex);
    return _tasks.empty();
}

Signal<>::Subscription MainThreadQueue::subscribe(Signal<>::Callback callback)
{
    auto subscription = _posted.subscribe(callback);
    bool notify_now;
    {
        std::lock_guard lock(_mutex);
        notify_now = !_tasks.empty();
    }
    if (notify_now) {
        callback();
    }
    return subscription;
}

} // namespace ursa
