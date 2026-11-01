#include "acp/wire.h"

namespace imza::acp {

namespace {

    constexpr std::string_view JSONRPC = "2.0";

    JsonValue object() { return JsonValue(JsonValue::object_t { }); }

} // namespace

bool parse_frame(std::string_view line, Message& out)
{
    out = Message { };
    JsonValue parsed;
    if (!json_parse(line, parsed)) {
        return false;
    }
    const JsonValue* version = find_member(parsed, "jsonrpc");
    if (version == nullptr || !version->is_string()
        || version->get<std::string>() != JSONRPC) {
        return false;
    }
    if (const JsonValue* id = find_member(parsed, "id"); id != nullptr) {
        // Numbers and strings are both valid JSON-RPC ids; Zed sends
        // UUID strings. Anything else is malformed.
        if (!id->is_number() && !id->is_string()) {
            return false;
        }
        out.id = *id;
    }
    if (const JsonValue* method = find_member(parsed, "method");
        method != nullptr && method->is_string()) {
        out.method = method->get<std::string>();
    }
    if (const JsonValue* params = find_member(parsed, "params");
        params != nullptr && (params->is_object() || params->is_array())) {
        out.params = *params;
    }
    if (const JsonValue* error = find_member(parsed, "error");
        error != nullptr && error->is_object()) {
        out.error = *error;
    }
    if (const JsonValue* result = find_member(parsed, "result");
        result != nullptr) {
        out.result = *result;
    }
    // Either a request/notification (method present) or a response
    // (result or error present).
    return !out.method.empty() || out.result.has_value()
        || out.error.has_value();
}

std::string dump_request(
    const JsonValue& id, std::string_view method, const JsonValue& params)
{
    JsonValue frame;
    frame["jsonrpc"] = std::string(JSONRPC);
    frame["id"]      = id;
    frame["method"]  = std::string(method);
    frame["params"]  = params.is_null() ? object() : params;
    return json_dump(frame);
}

std::string dump_response(const JsonValue& id, const JsonValue& result)
{
    JsonValue frame;
    frame["jsonrpc"] = std::string(JSONRPC);
    frame["id"]      = id;
    frame["result"]  = result.is_null() ? object() : result;
    return json_dump(frame);
}

std::string dump_notification(std::string_view method, const JsonValue& params)
{
    JsonValue frame;
    frame["jsonrpc"] = std::string(JSONRPC);
    frame["method"]  = std::string(method);
    frame["params"]  = params.is_null() ? object() : params;
    return json_dump(frame);
}

std::string dump_error(const JsonValue& id, int code, std::string_view message)
{
    JsonValue frame;
    frame["jsonrpc"] = std::string(JSONRPC);
    frame["id"]      = id;
    JsonValue error;
    error["code"]    = static_cast<double>(code);
    error["message"] = std::string(message);
    frame["error"]   = error;
    return json_dump(frame);
}

JsonValue text_block(std::string_view text)
{
    JsonValue block;
    block["type"] = "text";
    block["text"] = std::string(text);
    return block;
}

JsonValue session_update(std::string_view session_id, JsonValue update)
{
    JsonValue params;
    params["sessionId"] = std::string(session_id);
    params["update"]    = std::move(update);
    return params;
}

JsonValue update_of(std::string_view kind)
{
    JsonValue update;
    update["sessionUpdate"] = std::string(kind);
    return update;
}

} // namespace imza::acp
