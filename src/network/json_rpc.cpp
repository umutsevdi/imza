#include "network/json_rpc.h"

namespace imza {

namespace {

    constexpr std::string_view JSONRPC = "2.0";

    JsonValue params_or_object(const JsonValue& params)
    {
        return params.is_null() ? json_object() : params;
    }

} // namespace

bool parse_rpc_frame(std::string_view line, RpcFrame& out)
{
    out = RpcFrame { };
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
        // Numbers and strings are both valid ids; anything else is
        // malformed.
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

std::string rpc_request(
    const JsonValue& id, std::string_view method, const JsonValue& params)
{
    JsonValue frame;
    frame["jsonrpc"] = std::string(JSONRPC);
    frame["id"]      = id;
    frame["method"]  = std::string(method);
    frame["params"]  = params_or_object(params);
    return json_dump(frame);
}

std::string rpc_response(const JsonValue& id, const JsonValue& result)
{
    JsonValue frame;
    frame["jsonrpc"] = std::string(JSONRPC);
    frame["id"]      = id;
    frame["result"]  = params_or_object(result);
    return json_dump(frame);
}

std::string rpc_notification(std::string_view method, const JsonValue& params)
{
    JsonValue frame;
    frame["jsonrpc"] = std::string(JSONRPC);
    frame["method"]  = std::string(method);
    frame["params"]  = params_or_object(params);
    return json_dump(frame);
}

std::string rpc_error(const JsonValue& id, int code, std::string_view message)
{
    JsonValue frame;
    frame["jsonrpc"] = std::string(JSONRPC);
    frame["id"]      = id;
    JsonValue error;
    error["code"]    = static_cast<double>(code);
    error["message"] = std::string(message);
    frame["error"]   = std::move(error);
    return json_dump(frame);
}

} // namespace imza
