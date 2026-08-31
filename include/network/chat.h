#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "common/tool_call.h"

namespace ursa {

struct ThinkingBlock {
    std::string text;
    std::string signature;
};

struct Message {
    enum class Type { SYSTEM, USER, ASSISTANT, TOOL };
    Type type;
    std::string content;
    std::vector<ToolCallEntry> tool_calls { };
    std::string tool_call_id { };
    std::vector<ThinkingBlock> thinking { };
};

struct Usage {
    std::uint64_t prompt       = 0;
    std::uint64_t completion   = 0;
    std::uint64_t cached_read  = 0;
    std::uint64_t cached_write = 0;
    std::uint64_t total        = 0;
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

} // namespace ursa
