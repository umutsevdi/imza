#pragma once

#include <functional>
#include <json/json.h>
#include <string>
#include <vector>

#include "types.hpp"

namespace ursa {

struct Message {
    enum class Type { SYSTEM, USER, ASSISTANT };
    Type type;
    std::string content;
};

struct StreamEvent {
    enum class Kind { CONTENT_DELTA, DONE, ERROR };
    Kind kind;
    std::string text;
    Status error;
};

struct ChatRequest {
    std::string model;
    std::vector<Message> messages;
    double temperature = 0.7;
};

struct ChatResponse {
    std::string content;
    int prompt_tokens     = 0;
    int completion_tokens = 0;
};

using StreamCallback = std::function<void(const StreamEvent&)>;

Status chat_stream(
    const Config& cfg, const ChatRequest& req, StreamCallback on_event);

Json::Value build_request(Standard standard, const ChatRequest& req);
Status parse_stream_event(Standard standard, std::string_view event,
    std::string_view data, StreamEvent& out);

} // namespace ursa
