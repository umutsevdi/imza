#pragma once

#include <cstdint>
#include <json/json.h>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "types.h"

namespace ursa {

struct ToolSpec {
    std::string name;
    std::string description;
    Json::Value parameters;
};

struct ToolCallEntry {
    std::string id;
    std::string name;
    std::string args;
};

struct Message {
    enum class Type { SYSTEM, USER, ASSISTANT, TOOL };
    Type type;
    std::string content;
    std::vector<ToolCallEntry> tool_calls { };
    std::string tool_call_id { };
};

struct Usage {
    std::uint64_t prompt    = 0;
    std::uint64_t completion = 0;
    std::uint64_t total     = 0;
};

struct StreamEvent {
    enum class Kind { CONTENT_DELTA, TOOL_CALL, QUESTION, DONE, ERROR, USAGE, CONNECTED };
    Kind kind;
    std::string text;
    Status error;
    ToolCallRequest tool_call;
    QuestionForm question;
    Usage usage;
};

StreamEvent make_delta_event(std::string text);
StreamEvent make_tool_call_event(ToolCallRequest request);
StreamEvent make_question_event(QuestionForm form);
StreamEvent make_done_event();
StreamEvent make_error_event(Status error, std::string message = "");
StreamEvent make_usage_event(Usage usage);
StreamEvent make_connected_event();

struct ChatRequest {
    std::string model;
    std::vector<Message> messages;
    std::vector<ToolSpec> tools;
    double temperature = 0.7;
};

using StreamCallback = std::function<void(const StreamEvent&)>;

struct ToolAccum {
    std::string id;
    std::string name;
    std::string args;
};

struct ParseState {
    std::map<int, ToolAccum> tool_accums;
    Usage usage;
    bool usage_emitted = false;
};

struct Provider {
    Json::Value (*build)(const ChatRequest& req);
    std::string (*endpoint)();
    std::vector<std::string> (*headers)(const std::string& api_key);
    Status (*parse)(ParseState& state, std::string_view event,
        std::string_view data, std::vector<StreamEvent>& outs);
};

Provider get_provider(const Config& cfg);

Status stream(const Provider& provider, const Config& cfg,
    const ChatRequest& req, StreamCallback cb, int* retry_after = nullptr);

Status parse_api_error(std::string_view body, std::string& message);

std::string write_json(const Json::Value& value);
Json::Value parse_json(std::string_view text);
const char* role_str(Message::Type type);

} // namespace ursa
