#pragma once

#include <json/json.h>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
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

enum class AuthType { BEARER, ANTHROPIC, NONE };

struct Route {
    std::string endpoint;
    std::string api;
    ApiStandard dialect = ApiStandard::OPENAI;
    AuthType auth       = AuthType::BEARER;
    std::string api_key;
};

std::vector<std::string> auth_headers(AuthType auth, const std::string& key);

struct ModelInfo {
    std::string id;
    std::string name;
    std::optional<std::uint64_t> context_length;
};

Status parse_models_response(
    std::string_view body, std::vector<ModelInfo>& out);
Status fetch_models(const Route& route, std::vector<ModelInfo>& out);

Status http_get(const std::string& url, const std::vector<std::string>& headers,
    long timeout_secs, std::string& body, long* http_code);

using StreamCallback = std::function<void(const StreamEvent&)>;

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

Status stream(const Provider& provider, const Route& route,
    const ChatRequest& req, StreamCallback cb, int* retry_after = nullptr);

Status parse_api_error(std::string_view body, std::string& message);

std::string write_json(const Json::Value& value);
Json::Value parse_json(std::string_view text);
const char* role_str(Message::Type type);

} // namespace ursa
