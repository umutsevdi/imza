#include "agent/modal_queue.h"

#include <utility>

namespace ursa {

void ModalQueue::enqueue(
    ModalPayload payload, std::shared_ptr<std::promise<ModalResult>> promise)
{
    std::lock_guard lock(mutex_);
    entries_.push_back(PendingModal { std::move(payload), std::move(promise) });
}

std::optional<PendingModal> ModalQueue::try_pop()
{
    std::lock_guard lock(mutex_);
    if (entries_.empty()) {
        return std::nullopt;
    }
    PendingModal entry = std::move(entries_.front());
    entries_.pop_front();
    return entry;
}

std::optional<ModalPayload> ModalQueue::peek_front() const
{
    std::lock_guard lock(mutex_);
    if (entries_.empty()) {
        return std::nullopt;
    }
    return entries_.front().payload;
}

std::size_t ModalQueue::size() const
{
    std::lock_guard lock(mutex_);
    return entries_.size();
}

void ModalQueue::clear()
{
    std::lock_guard lock(mutex_);
    entries_.clear();
}

void ModalQueue::abandon()
{
    std::lock_guard lock(mutex_);
    for (auto& entry : entries_) {
        if (entry.promise) {
            entry.promise->set_value(std::monostate { });
        }
    }
    entries_.clear();
}

} // namespace ursa
