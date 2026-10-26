#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "common/tool_call.h"
#include "common/types.h"

namespace imza {

struct ThinkingBlock {
    std::string text;
    std::string signature;
};

struct Message {
    enum class Type { SYSTEM, USER, ASSISTANT, TOOL };
    Type type;
    std::string content;
    std::vector<ToolCallEntry> tool_calls;
    std::string tool_call_id;
    std::vector<ThinkingBlock> thinking;
    std::vector<Attachment> media;
};

struct Usage {
    std::uint64_t prompt       = 0;
    std::uint64_t completion   = 0;
    std::uint64_t cached_read  = 0;
    std::uint64_t cached_write = 0;
    std::uint64_t total        = 0;
};

// One provider request's usage, recorded per assistant turn and in the
// session's analytics log. Provider identity rides along so global views
// can group by vendor (and subscription vs pay-per-token), not just model.
// The epoch-0 default marks entries that predate per-request timestamps;
// per-day views fall back to their turn or save day.
struct UsageEntry {
    std::string provider;
    std::string model;
    Usage usage;
    double cost = 0.0;
    std::chrono::system_clock::time_point finished_at { };
};

struct ChatRequest {
    std::string model;
    std::vector<Message> messages;
    std::vector<ToolSpec> tools;
    double temperature = 0.7;
    std::optional<std::string> reasoning_effort;
    std::optional<std::uint64_t> thinking_budget;
    std::optional<std::uint64_t> max_output_tokens;
    std::function<bool()> interrupted;
};

const char* role_str(Message::Type type);

// Maps a configured reasoning effort ("off"/"low"/"default"/"high") onto
// the request's wire fields for one dialect: Anthropic takes a thinking
// token budget, every other dialect takes the effort string. The caller
// gates on whether the model reasons at all.
void apply_reasoning_dialect(
    ChatRequest& request, ApiStandard dialect, std::string_view effort);

} // namespace imza
