#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/tool_call.h"
#include "common/types.h"
#include "network/chat.h"

namespace imza {

struct StreamEvent {
    enum class Kind {
        CONTENT_DELTA,
        TOOL_CALL_START,
        TOOL_CALL,
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
    Usage usage { };
    std::string thinking_signature;
};

StreamEvent make_delta_event(std::string text);
StreamEvent make_tool_call_start_event(ToolCallRequest request);
StreamEvent make_tool_call_event(ToolCallRequest request);
StreamEvent make_done_event();
StreamEvent make_error_event(Status error, std::string message = "");
StreamEvent make_usage_event(Usage usage);
StreamEvent make_connected_event();
StreamEvent make_reasoning_event(std::string text, std::string signature = "");

enum class AuthType { BEARER, ANTHROPIC, OPENAI_SUBSCRIPTION, NONE };

struct Route {
    std::string endpoint;
    std::string api;
    ApiStandard dialect = ApiStandard::OPENAI;
    AuthType auth       = AuthType::BEARER;
    std::string api_key;
    std::string account_id;
    std::string user_agent;
    std::string opencode_session;
    Status error = Status::OK;
    std::string error_message;
};

std::vector<std::string> auth_headers(
    AuthType auth, const std::string& key, const std::string& account_id = { });

std::vector<std::string> request_headers(
    const Route& route, std::vector<std::string> base);

struct HttpGetOptions {
    std::size_t max_bytes = 0;
    bool* truncated       = nullptr;
    long max_redirs       = 5;
};

struct HttpPostOptions {
    long max_redirs = 5;
    // Raw response header lines ("Name: value") when non-null; HTTP/1.x
    // status lines are excluded.
    std::vector<std::string>* response_headers = nullptr;
};

Status http_get(const std::string& url, const std::vector<std::string>& headers,
    long timeout_secs, std::string& body, long* http_code,
    const HttpGetOptions& opts = { });

Status http_post(const std::string& url,
    const std::vector<std::string>& headers, const std::string& payload,
    long timeout_secs, std::string& body, long* http_code,
    const HttpPostOptions& opts = { });

// Best-effort DELETE; the response body is discarded.
Status http_delete(const std::string& url,
    const std::vector<std::string>& headers, long timeout_secs,
    long* http_code);

inline bool http_ok(long code) { return code >= 200 && code < 300; }

using StreamCallback = std::function<void(const StreamEvent&)>;

Status stream(const Route& route, const ChatRequest& req, StreamCallback cb,
    int* retry_after = nullptr);

Status parse_api_error(std::string_view body, std::string& message);

Status classify_failure(
    long code, const std::string& raw, std::string& message);

enum class Capabilities : std::uint8_t {
    NONE  = 0,
    IMAGE = 2 << 0,
    PDF   = 2 << 1,
};

constexpr Capabilities operator|(Capabilities lhs, Capabilities rhs)
{
    return static_cast<Capabilities>(
        static_cast<std::uint8_t>(lhs) | static_cast<std::uint8_t>(rhs));
}

constexpr bool has_capability(
    Capabilities capabilities, Capabilities capability)
{
    return (static_cast<std::uint8_t>(capabilities)
               & static_cast<std::uint8_t>(capability))
        != 0;
}

struct ModelInfo {
    std::string id;
    std::string name;
    std::optional<std::uint64_t> context_length;
    std::optional<Capabilities> capabilities;
};

Status parse_models_response(
    std::string_view body, std::vector<ModelInfo>& out);
Status fetch_models(const Route& route, std::vector<ModelInfo>& out);

} // namespace imza
