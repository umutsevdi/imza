#include "turn/turn_runner.h"

#include <chrono>
#include <mutex>
#include <utility>
#include <vector>

namespace ursa {

namespace {

    constexpr auto UPDATE_INTERVAL = std::chrono::milliseconds { 20 };

    struct PendingUpdate {
        StreamEvent event;
        ModelPricing pricing;
    };

} // namespace

struct StreamUpdateBuffer::State {
    State(PostFn next_post, std::shared_ptr<Session> next_session)
        : post(std::move(next_post))
        , session(std::move(next_session))
    {
    }

    PostFn post;
    std::shared_ptr<Session> session;
    std::mutex mutex;
    std::vector<PendingUpdate> updates;
    bool task_pending = false;
};

StreamUpdateBuffer::StreamUpdateBuffer(
    PostFn post, std::shared_ptr<Session> session)
    : _state(std::make_shared<State>(std::move(post), std::move(session)))
    , _publisher([state = _state](const std::stop_token& stop) {
        while (!stop.stop_requested()) {
            std::this_thread::sleep_for(UPDATE_INTERVAL);
            if (!stop.stop_requested()) {
                _publish(state);
            }
        }
    })
{
}

StreamUpdateBuffer::~StreamUpdateBuffer() { finish(); }

void StreamUpdateBuffer::_publish(const std::shared_ptr<State>& state)
{
    {
        std::lock_guard lock(state->mutex);
        if (state->updates.empty() || state->task_pending) {
            return;
        }
        state->task_pending = true;
    }
    state->post([state] {
        std::vector<PendingUpdate> updates;
        {
            std::lock_guard lock(state->mutex);
            updates.swap(state->updates);
            state->task_pending = false;
        }
        for (const PendingUpdate& update : updates) {
            state->session->apply(update.event, update.pricing);
        }
    });
}

void StreamUpdateBuffer::push(
    const StreamEvent& event, const ModelPricing& pricing)
{
    std::lock_guard lock(_state->mutex);
    if (!_state->updates.empty()
        && _state->updates.back().event.kind == event.kind
        && (event.kind == StreamEvent::Kind::CONTENT_DELTA
            || event.kind == StreamEvent::Kind::REASONING)) {
        StreamEvent& pending = _state->updates.back().event;
        pending.text += event.text;
        if (!event.thinking_signature.empty()) {
            pending.thinking_signature = event.thinking_signature;
        }
        return;
    }
    _state->updates.push_back(PendingUpdate { event, pricing });
}

void StreamUpdateBuffer::finish()
{
    if (_publisher.joinable()) {
        _publisher.request_stop();
        _publisher.join();
    }
    _publish(_state);
}

} // namespace ursa
