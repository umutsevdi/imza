#include "turn/stream_updates.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <utility>
#include <vector>

namespace imza {

namespace {

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
    std::condition_variable ready;
    std::vector<PendingUpdate> updates;
    bool task_pending = false;
};

StreamUpdateBuffer::StreamUpdateBuffer(
    PostFn post, std::shared_ptr<Session> session)
    : _state(std::make_shared<State>(std::move(post), std::move(session)))
    , _publisher([state = _state](const std::stop_token& stop) {
        std::unique_lock lock(state->mutex);
        while (true) {
            // Debounce: a burst of deltas arriving faster than the drain
            // applies them must coalesce into one posted task, not one per
            // push, so pushes do not wake this loop; the tick bounds update
            // latency instead.
            state->ready.wait_for(lock, std::chrono::milliseconds(10),
                [&stop] { return stop.stop_requested(); });
            if (stop.stop_requested()) {
                return;
            }
            lock.unlock();
            _publish(state);
            lock.lock();
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
        state->ready.notify_all();
    });
}

void StreamUpdateBuffer::push(
    const StreamEvent& event, const ModelPricing& pricing)
{
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
}

void StreamUpdateBuffer::finish()
{
    if (_publisher.joinable()) {
        _publisher.request_stop();
        {
            std::lock_guard lock(_state->mutex);
        }
        _state->ready.notify_all();
        _publisher.join();
    }
    _publish(_state);
}

} // namespace imza
