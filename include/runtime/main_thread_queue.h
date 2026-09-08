#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>

#include "common/types.h"
#include "common/imza_signal.h"

namespace imza {

class MainThreadQueue final : public ApplicationComponent {
public:
    using Task = std::function<void()>;

    void post(Task task);
    void drain();
    bool wait_for_task(std::chrono::milliseconds timeout);

    [[nodiscard]] Signal<>::Subscription subscribe(Signal<>::Callback callback);

private:
    mutable std::mutex _mutex;
    std::condition_variable _ready;
    std::deque<Task> _tasks;
    Signal<> _posted;
};

} // namespace imza
