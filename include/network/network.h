#pragma once

#include <json/json.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "network/chat.h"
#include "common/modal.h"
#include "common/tool_call.h"
#include "common/types.h"

namespace ursa {

struct StreamEvent {
    enum class Kind {
        CONTENT_DELTA,
        TOOL_CALL,
        QUESTION,
        DONE,
        ERROR,
        USAGE,
        CONNECTED,
        REASONING
    };
    Kind kind = Kind::CONTENT_DELTA;
    std::string text;
    Status error = Status::OK;
    ToolCallRequest tool_call;
    QuestionForm question;
    Usage usage { };
    std::string thinking_signature { };
};

StreamEvent make_delta_event(std::string text);
StreamEvent make_tool_call_event(ToolCallRequest request);
StreamEvent make_question_event(QuestionForm form);
StreamEvent make_done_event();
StreamEvent make_error_event(Status error, std::string message = "");
StreamEvent make_usage_event(Usage usage);
StreamEvent make_connected_event();
StreamEvent make_reasoning_event(std::string text, std::string signature = "");

enum class AuthType { BEARER, ANTHROPIC, NONE };

struct Route {
    std::string endpoint;
    std::string api;
    ApiStandard dialect = ApiStandard::OPENAI;
    AuthType auth       = AuthType::BEARER;
    std::string api_key;
};

std::vector<std::string> auth_headers(AuthType auth, const std::string& key);

Status http_get(const std::string& url, const std::vector<std::string>& headers,
    long timeout_secs, std::string& body, long* http_code);

using StreamCallback = std::function<void(const StreamEvent&)>;

Status stream(const Route& route, const ChatRequest& req, StreamCallback cb,
    int* retry_after = nullptr);

Status parse_api_error(std::string_view body, std::string& message);

} // namespace ursa
