#pragma once

#include <memory>
#include <thread>

#include "conversation/session.h"
#include "network/network.h"
#include "providers/pricing.h"
#include "runtime/callbacks.h"

namespace imza {

// Bounded transcript shared by automatic compaction and sidechat seeding.
std::string conversation_transcript(
    const std::string& compacted_summary, const SessionSnapshot& snapshot);

// Coalesces streamed events and publishes session updates at a bounded
// rate from a dedicated publisher thread.
class StreamUpdateBuffer {
public:
    StreamUpdateBuffer(PostFn post, std::shared_ptr<Session> session);
    ~StreamUpdateBuffer();

    StreamUpdateBuffer(const StreamUpdateBuffer&)            = delete;
    StreamUpdateBuffer& operator=(const StreamUpdateBuffer&) = delete;

    void push(const StreamEvent& event, const ModelPricing& pricing);
    void finish();

private:
    struct State;
    static void _publish(const std::shared_ptr<State>& state);
    std::shared_ptr<State> _state;
    std::jthread _publisher;
};

} // namespace imza
