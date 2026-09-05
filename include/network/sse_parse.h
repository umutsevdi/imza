#pragma once

#include <json/json.h>

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "common/tool_call.h"
#include "network/chat.h"
#include "network/network.h"

namespace ursa {

struct ToolAccum {
    std::string id;
    std::string name;
    std::string args;
};

ToolCallRequest finish_accum(const ToolAccum& acc);

struct ThinkingAccum {
    std::string text;
    std::string signature;
};

struct ParseState {
    std::map<int, ToolAccum> tool_accums;
    std::map<int, ThinkingAccum> thinking_accums;
    Usage usage;
    bool usage_emitted = false;
    bool terminal      = false;
};

std::vector<std::string> stream_headers();
void flush_tool_accums(ParseState& state, std::vector<StreamEvent>& outs);

struct Provider {
    Json::Value (*build)(const ChatRequest& req);
    std::vector<std::string> (*headers)();
    void (*parse)(ParseState& state, std::string_view event,
        std::string_view data, std::vector<StreamEvent>& outs);
};

Provider get_provider(const Route& route);

} // namespace ursa
