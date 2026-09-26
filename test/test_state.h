#pragma once

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "app/application_state.h"

namespace imza::test {

// A fake main-thread queue: fn() produces the PostFn wiring, pump() runs
// everything posted so far, and wait_for() pumps until the predicate holds
// or a generous deadline expires.
class PostPump {
public:
    imza::PostFn fn()
    {
        return [this](std::function<void()> task) { push(std::move(task)); };
    }

    void pump()
    {
        for (;;) {
            std::function<void()> task;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (queue_.empty()) {
                    return;
                }
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            task();
        }
    }

    template <typename Pred> bool wait_for(Pred pred)
    {
        for (int i = 0; i < 20000; ++i) {
            pump();
            if (pred()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }

private:
    void push(std::function<void()> task)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(task));
    }

    std::mutex mutex_;
    std::deque<std::function<void()>> queue_;
};

// One provider/selection so submit() reaches the test's stream stub.
inline imza::Config test_config()
{
    imza::Config cfg;
    imza::Connection conn;
    conn.id = "test";
    cfg.providers.push_back(conn);
    cfg.last_used = imza::LastUsed { "test", "m" };
    return cfg;
}

// The calling thread is the main thread: posted closures run inline.
inline void run_immediately(std::function<void()> task) { task(); }

// Root application state built through the production factory, so the tool
// roster and the gated lua host come from application_state.cpp instead of
// a test copy of its wiring.
inline std::shared_ptr<imza::ApplicationState> make_test_state(
    imza::PostFn post, imza::Config config = { }, imza::StreamFn stream = { },
    imza::RuntimeFlag flags = imza::interactive_runtime_flags())
{
    return imza::make_application_state(
        std::move(post), std::move(config), std::move(stream), flags);
}

// Empty-config interactive state with an immediate main thread: the shape
// most flow and UI tests want.
inline std::shared_ptr<imza::ApplicationState> make_test_state()
{
    return make_test_state(run_immediately);
}

// Turn finished and nothing awaits a modal.
inline bool idle(const imza::Session& session)
{
    return session.phase() == imza::Session::Phase::IDLE
        && session.modal().index() == 0;
}

} // namespace imza::test
