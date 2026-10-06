#pragma once

#include "network/json.h"
#include "network/network.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace imza {

// Minimal MCP client core (Model Context Protocol, 2025-11-25): the
// initialize handshake plus the tools surface over the Streamable HTTP
// transport. Every call is one blocking HTTP round trip on the calling
// thread; sequencing and thread choice belong to the caller. Requests are
// serialized per session, so JSON-RPC ids need no correlation map.

inline constexpr std::string_view MCP_PROTOCOL_VERSION = "2025-11-25";

// Wire subset: unknown members are tolerated on read (JSON_READ) so
// servers may add fields. Binary and resource content blocks parse but are
// not surfaced; callers consume text.

struct McpServerInfo {
    std::string name;
    std::optional<std::string> title;
    std::optional<std::string> version;
};

struct McpInitializeResult {
    std::string protocol_version; // "protocolVersion"
    std::optional<McpServerInfo> server_info;
    std::optional<std::string> instructions;
};

struct McpContentItem {
    std::optional<std::string> type;
    std::optional<std::string> text;
};

struct McpToolCallResult {
    std::optional<std::vector<McpContentItem>> content;
    std::optional<bool> is_error; // "isError": tool execution error
    std::optional<JsonValue> structured_content; // "structuredContent"
};

struct McpToolDefinition {
    std::string name;
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::optional<JsonValue> input_schema; // "inputSchema", verbatim
};

struct McpListToolsResult {
    std::optional<std::vector<McpToolDefinition>> tools;
    std::optional<std::string> next_cursor; // "nextCursor"
};

struct McpRpcError {
    std::int64_t code = 0;
    std::string message;
};

struct McpEndpoint {
    std::string url;                  // MCP endpoint URL (Streamable HTTP)
    std::string bearer_token;         // optional "Authorization: Bearer" value
    std::vector<std::string> headers; // static extra headers
    long timeout_secs = 25;
};

// One conversation with one server. Not thread-safe; keep it on the
// calling thread.
struct McpSession {
    McpEndpoint endpoint;
    std::string session_id; // "MCP-Session-Id", once the server assigns one
    std::string protocol_version = std::string(MCP_PROTOCOL_VERSION);
    std::uint64_t next_id        = 0;
    bool initialized             = false;
};

// Serializes {"jsonrpc":"2.0","id":id,"method":method,"params":params}.
std::string mcp_rpc_request(
    std::uint64_t id, std::string_view method, const JsonValue& params);

// True when the client can speak a server-proposed protocol version.
bool mcp_version_supported(std::string_view version);

// Extracts the JSON-RPC response message from a POST body: a top-level
// JSON object carrying "result" or "error", or the first SSE "data:" line
// carrying one. The matched message is copied into `out`.
bool mcp_find_rpc_response(std::string_view body, std::string& out);

// Value of the "MCP-Session-Id" header among raw response header lines
// (case-insensitive); "" when absent.
std::string mcp_session_id(const std::vector<std::string>& headers);

// Runs the initialize handshake and sends notifications/initialized;
// fills session_id and the negotiated protocol_version. Fails closed on
// an unsupported version.
Status mcp_initialize(McpSession& session, std::string& detail);

// tools/call. Tool execution errors (isError) are reported through the
// result; JSON-RPC protocol errors as Status::API_ERROR with `detail`.
// Re-initializes once when the server expired the session (HTTP 404).
Status mcp_call_tool(McpSession& session, const std::string& name,
    const JsonValue& arguments, McpToolCallResult& out, std::string& detail);

// tools/list with full cursor pagination; appends to `out`.
Status mcp_list_tools(McpSession& session, std::vector<McpToolDefinition>& out,
    std::string& detail);

// Best-effort DELETE of the remote session; always clears session state.
void mcp_end_session(McpSession& session);

// First non-empty text block of a tool result; "" when it carries none.
std::string mcp_first_text(const McpToolCallResult& result);

} // namespace imza

// Wire keys are camelCase; the mappings live here so every TU that reads
// these structs through glaze agrees on them.
template <> struct glz::meta<imza::McpInitializeResult> {
    using T = imza::McpInitializeResult;
    static constexpr auto value
        = glz::object("protocolVersion", &T::protocol_version, "serverInfo",
            &T::server_info, "instructions", &T::instructions);
};

template <> struct glz::meta<imza::McpToolCallResult> {
    using T                     = imza::McpToolCallResult;
    static constexpr auto value = glz::object("content", &T::content, "isError",
        &T::is_error, "structuredContent", &T::structured_content);
};

template <> struct glz::meta<imza::McpToolDefinition> {
    using T = imza::McpToolDefinition;
    static constexpr auto value
        = glz::object("name", &T::name, "title", &T::title, "description",
            &T::description, "inputSchema", &T::input_schema);
};

template <> struct glz::meta<imza::McpListToolsResult> {
    using T = imza::McpListToolsResult;
    static constexpr auto value
        = glz::object("tools", &T::tools, "nextCursor", &T::next_cursor);
};
