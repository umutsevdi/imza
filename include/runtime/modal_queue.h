#pragma once

#include <cstddef>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>

#include "common/modal.h"
#include "common/types.h"

namespace imza {

enum class ModalOrigin { USER, AGENT };

struct PendingModal {
    ModalPayload payload;
    std::shared_ptr<std::promise<ModalResult>> promise;
    ModalOrigin origin = ModalOrigin::USER;
};

class ModalQueue final : public ApplicationComponent {
public:
    void enqueue(ModalPayload payload, ModalOrigin origin,
        std::shared_ptr<std::promise<ModalResult>> promise = { });
    std::optional<PendingModal> try_pop();
    std::optional<PendingModal> peek_front() const;
    std::size_t size() const;
    void clear();
    void abandon();

private:
    mutable std::mutex mutex_;
    std::deque<PendingModal> entries_;
};

} // namespace imza
