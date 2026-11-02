#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "network/json.h"

namespace imza {

// JSON-RPC 2.0 framing shared by every line- or body-oriented protocol
// adapter (the MCP client, the ACP server). One envelope implementation:
// protocols differ in their methods and payloads, never in the frame.

// Standard JSON-RPC codes. Protocol-specific codes (ACP's -32002, MCP's
// own range) stay with their protocol.
inline constexpr int RPC_PARSE_ERROR      = -32700;
inline constexpr int RPC_INVALID_REQUEST  = -32600;
inline constexpr int RPC_METHOD_NOT_FOUND = -32601;
inline constexpr int RPC_INVALID_PARAMS   = -32602;
inline constexpr int RPC_INTERNAL_ERROR   = -32603;

// One decoded frame: a request/notification (method present) or a
// response (result or error present). Ids stay raw JSON because peers
// send both numbers and strings and expect them echoed verbatim.
struct RpcFrame {
    JsonValue id;
    std::optional<JsonValue> result;
    std::optional<JsonValue> error;
    std::string method;
    JsonValue params;
};

// False for malformed lines, which the caller drops.
bool parse_rpc_frame(std::string_view line, RpcFrame& out);

// One frame serialized as a single line, without a trailing newline.
// A null params/result becomes an empty object: strict peers reject
// "params":null as an invalid JSON-RPC message.
std::string rpc_request(
    const JsonValue& id, std::string_view method, const JsonValue& params);
std::string rpc_response(const JsonValue& id, const JsonValue& result);
std::string rpc_notification(std::string_view method, const JsonValue& params);
std::string rpc_error(const JsonValue& id, int code, std::string_view message);

} // namespace imza
