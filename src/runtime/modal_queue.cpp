#include "runtime/modal_queue.h"

#include <utility>

namespace imza {

void ModalQueue::enqueue(ModalPayload payload, ModalOrigin origin,
    std::shared_ptr<std::promise<ModalResult>> promise)
{
    std::lock_guard lock(_mutex);
    _entries.push_back(
        PendingModal { std::move(payload), std::move(promise), origin });
}

std::optional<PendingModal> ModalQueue::try_pop()
{
    std::lock_guard lock(_mutex);
    if (_entries.empty()) {
        return std::nullopt;
    }
    PendingModal entry = std::move(_entries.front());
    _entries.pop_front();
    return entry;
}

std::optional<PendingModal> ModalQueue::peek_front() const
{
    std::lock_guard lock(_mutex);
    if (_entries.empty()) {
        return std::nullopt;
    }
    return _entries.front();
}

std::size_t ModalQueue::size() const
{
    std::lock_guard lock(_mutex);
    return _entries.size();
}

void ModalQueue::clear()
{
    std::lock_guard lock(_mutex);
    _entries.clear();
}

void ModalQueue::abandon()
{
    std::lock_guard lock(_mutex);
    for (auto& entry : _entries) {
        if (entry.promise) {
            entry.promise->set_value(std::monostate { });
        }
    }
    _entries.clear();
}

} // namespace imza
