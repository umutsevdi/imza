#pragma once

#include <functional>
#include <future>

#include "common/modal.h"
#include "network/network.h"

namespace imza {

enum class AgentNotification {
    TURN_FINISHED,
    INPUT_REQUIRED,
    REVIEW_PASS_FINISHED
};

using PostFn = std::function<void(std::function<void()>)>;
using StreamFn
    = std::function<Status(const ChatRequest&, const StreamCallback&)>;
using ModalRequestFn = std::function<std::future<ModalResult>(ModalPayload)>;

} // namespace imza
