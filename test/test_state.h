#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

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
                std::lock_guard<std::mutex> lock(_mutex);
                if (_queue.empty()) {
                    return;
                }
                task = std::move(_queue.front());
                _queue.pop_front();
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
        std::lock_guard<std::mutex> lock(_mutex);
        _queue.push_back(std::move(task));
    }

    std::mutex _mutex;
    std::deque<std::function<void()>> _queue;
};

// Poll a predicate with 1ms sleeps for up to 20 seconds; on timeout the
// test fails loudly instead of the suite hanging on a busy-wait loop.
template <typename Pred> bool wait_until(Pred pred)
{
    for (int i = 0; i < 20000; ++i) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    FAIL("timed out waiting for a condition");
    return false;
}

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

// Turn-runner harness: a pumped main-thread queue, a recorded-request
// stream stub whose behavior is assigned after construction, and
// application state whose environment detection has completed.
struct AgentEnv {
    PostPump pump;
    std::vector<imza::ChatRequest> requests;
    imza::StreamFn stream;
    std::shared_ptr<imza::ApplicationState> state;
    std::shared_ptr<imza::Session> session;

    explicit AgentEnv(
        imza::RuntimeFlag flags = imza::interactive_runtime_flags())
        : state(make_test_state(
              pump.fn(), test_config(),
              [this](const imza::ChatRequest& req,
                  const imza::StreamCallback& cb) { return stream(req, cb); },
              flags))
        , session(state->session)
    {
        REQUIRE(pump.wait_for([&] { return state->environment->ready(); }));
    }

    const imza::ChatRequest& last_request() const { return requests.back(); }

    std::size_t user_turn_count() const
    {
        std::size_t count = 0;
        for (const auto& item : session->items()) {
            if (std::holds_alternative<imza::UserTurn>(item)) {
                ++count;
            }
        }
        return count;
    }

    const imza::ToolCall* pending_tool() const
    {
        for (auto it = session->items().rbegin(); it != session->items().rend();
            ++it) {
            if (const auto* call = std::get_if<imza::ToolCall>(&*it)) {
                return call;
            }
        }
        return nullptr;
    }
};

} // namespace imza::test
