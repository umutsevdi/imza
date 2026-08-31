#pragma once

#include <cstddef>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>

#include "common/modal.h"

namespace ursa {

struct PendingModal {
    ModalPayload payload;
    std::shared_ptr<std::promise<ModalResult>> promise;
};

class ModalQueue {
public:
    void enqueue(ModalPayload payload,
        std::shared_ptr<std::promise<ModalResult>> promise = { });
    std::optional<PendingModal> try_pop();
    std::optional<ModalPayload> peek_front() const;
    std::size_t size() const;
    void clear();
    void abandon();

private:
    mutable std::mutex mutex_;
    std::deque<PendingModal> entries_;
};

} // namespace ursa
