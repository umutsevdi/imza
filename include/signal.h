#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace ursa {

template <typename... Args> class Signal {
public:
    using Callback = std::function<void(Args...)>;

    Signal()                         = default;
    Signal(const Signal&)            = delete;
    Signal& operator=(const Signal&) = delete;
    Signal(Signal&&)                 = delete;
    Signal& operator=(Signal&&)      = delete;

private:
    struct Subscriber {
        std::uint64_t id = 0;
        Callback callback;
    };

    struct State {
        std::mutex mutex;
        std::vector<Subscriber> subscribers;
        std::uint64_t next_id = 1;
    };

public:
    class [[nodiscard]] Subscription {
    public:
        Subscription() = default;

        ~Subscription() { disconnect(); }

        Subscription(Subscription&& other) noexcept
            : _state(std::move(other._state))
            , _id(std::exchange(other._id, 0))
        {
        }

        Subscription& operator=(Subscription&& other) noexcept
        {
            if (this == &other) {
                return *this;
            }
            disconnect();
            _state = std::move(other._state);
            _id    = std::exchange(other._id, 0);
            return *this;
        }

        Subscription(const Subscription&)            = delete;
        Subscription& operator=(const Subscription&) = delete;

        void disconnect()
        {
            const std::shared_ptr<State> state = _state.lock();
            if (state && _id != 0) {
                std::lock_guard lock(state->mutex);
                std::erase_if(state->subscribers,
                    [id = _id](const Subscriber& subscriber) {
                        return subscriber.id == id;
                    });
            }
            _state.reset();
            _id = 0;
        }

    private:
        friend class Signal;

        Subscription(const std::shared_ptr<State>& state, std::uint64_t id)
            : _state(state)
            , _id(id)
        {
        }

        std::weak_ptr<State> _state;
        std::uint64_t _id = 0;
    };

    [[nodiscard]] Subscription subscribe(Callback callback)
    {
        std::lock_guard lock(_state->mutex);
        const std::uint64_t id = _state->next_id++;
        _state->subscribers.push_back(Subscriber { id, std::move(callback) });
        return Subscription(_state, id);
    }

    void publish(Args... args) const
    {
        std::vector<Callback> callbacks;
        {
            std::lock_guard lock(_state->mutex);
            callbacks.reserve(_state->subscribers.size());
            for (const Subscriber& subscriber : _state->subscribers) {
                callbacks.push_back(subscriber.callback);
            }
        }
        for (const Callback& callback : callbacks) {
            callback(args...);
        }
    }

private:
    std::shared_ptr<State> _state = std::make_shared<State>();
};

} // namespace ursa
