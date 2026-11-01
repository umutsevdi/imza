#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "network/json.h"

namespace imza::acp {

// One JSON-RPC 2.0 frame. Ids are numbers or strings; Zed sends UUID
// strings, so the id stays a raw JsonValue and is echoed back verbatim.
struct Message {
    JsonValue id;
    std::optional<JsonValue> result;
    std::optional<JsonValue> error;
    std::string method;
    JsonValue params;
};

// False for malformed lines, which the caller drops.
bool parse_frame(std::string_view line, Message& out);

// One frame serialized as a single line, without the trailing newline.
std::string dump_request(
    const JsonValue& id, std::string_view method, const JsonValue& params);
std::string dump_response(const JsonValue& id, const JsonValue& result);
std::string dump_notification(std::string_view method, const JsonValue& params);
std::string dump_error(const JsonValue& id, int code, std::string_view message);

inline constexpr int ERR_METHOD_NOT_FOUND   = -32601;
inline constexpr int ERR_INTERNAL_ERROR     = -32603;
inline constexpr int ERR_RESOURCE_NOT_FOUND = -32002;

JsonValue text_block(std::string_view text);
JsonValue session_update(std::string_view session_id, JsonValue update);
JsonValue update_of(std::string_view kind);

} // namespace imza::acp
