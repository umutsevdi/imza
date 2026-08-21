#pragma once

#include <json/json.h>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "types.h"

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

using StreamCallback = std::function<void(const StreamEvent&)>;

struct Provider {
    Json::Value (*build)(const ChatRequest& req);
    std::string (*endpoint)();
    std::vector<std::string> (*headers)(const std::string& api_key);
    Status (*parse)(
        std::string_view event, std::string_view data, StreamEvent& out);
};

Provider get_provider(const Config& cfg);

Status stream(const Provider& provider, const Config& cfg,
    const ChatRequest& req, StreamCallback cb);

std::string strip_slash(std::string_view base);
std::string write_json(const Json::Value& value);
Json::Value parse_json(std::string_view text);
const char* role_str(Message::Type type);

} // namespace ursa
