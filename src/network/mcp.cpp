#include "network/mcp.h"

#include "common/util.h"
#include "network/json_io.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <utility>

namespace imza {

// Glaze-reflected: must have external linkage (Clang/MSVC requirement).
struct McpInitializeResponse {
    std::optional<McpInitializeResult> result;
    std::optional<McpRpcError> error;
};

struct McpCallToolResponse {
    std::optional<McpToolCallResult> result;
    std::optional<McpRpcError> error;
};

struct McpListToolsResponse {
    std::optional<McpListToolsResult> result;
    std::optional<McpRpcError> error;
};

namespace {

    // Versions this client can speak; MCP_PROTOCOL_VERSION is proposed and
    // the server's counter-offer must land in this set.
    constexpr std::string_view KNOWN_VERSIONS[]
        = { "2025-03-26", "2025-06-18", "2025-11-25" };

    bool is_rpc_message(const JsonValue& value)
    {
        return value.is_object()
            && (find_member(value, "result") != nullptr
                || find_member(value, "error") != nullptr);
    }

    std::vector<std::string> auth_and_static_headers(const McpSession& session)
    {
        std::vector<std::string> headers = session.endpoint.headers;
        if (!session.endpoint.bearer_token.empty()) {
            headers.push_back(
                "Authorization: Bearer " + session.endpoint.bearer_token);
        }
        return headers;
    }

    // Negotiated requests carry the session and version headers; the
    // initialize request must not (nothing is negotiated yet).
    std::vector<std::string> post_headers(
        const McpSession& session, bool negotiated)
    {
        std::vector<std::string> headers = auth_and_static_headers(session);
        if (negotiated) {
            if (!session.session_id.empty()) {
                headers.push_back("MCP-Session-Id: " + session.session_id);
            }
            headers.push_back(
                "MCP-Protocol-Version: " + session.protocol_version);
        }
        headers.push_back("Content-Type: application/json");
        headers.push_back("Accept: application/json, text/event-stream");
        return headers;
    }

    // stdio exchange: write one line, then read until a JSON-RPC message
    // carrying result/error arrives, answering pings and skipping
    // notifications. http_code is 200 so callers' HTTP checks pass.
    Status post_stdio(const McpSession& session, const std::string& payload,
        std::string& body, long& http_code)
    {
        if (session.stdio.write(payload) != Status::OK) {
            return Status::NETWORK_ERROR;
        }
        http_code           = 200;
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(session.endpoint.timeout_secs);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto remaining
                = std::chrono::duration_cast<std::chrono::seconds>(
                    deadline - std::chrono::steady_clock::now());
            std::string line;
            const Status read = session.stdio.read(
                line, std::max<long>(1, remaining.count()));
            if (read == Status::TIMEOUT) {
                return Status::TIMEOUT;
            }
            if (read != Status::OK) {
                return Status::NETWORK_ERROR;
            }
            if (is_rpc_message(parse_json(line))) {
                body = std::move(line);
                return Status::OK;
            }
            // A server request or notification: answer pings, ignore the
            // rest, and keep reading for our response.
            const JsonValue message = parse_json(line);
            const JsonValue* method = find_member(message, "method");
            const JsonValue* id     = find_member(message, "id");
            if (method != nullptr && method->is_string() && id != nullptr
                && method->as<std::string>() == "ping") {
                JsonValue reply;
                reply["jsonrpc"] = "2.0";
                reply["id"]      = *id;
                reply["result"]  = JsonValue(JsonValue::object_t { });
                session.stdio.write(json_dump(reply));
            }
        }
        return Status::TIMEOUT;
    }

    Status post(McpSession& session, const std::string& payload,
        bool negotiated, long& http_code, std::string& body,
        std::vector<std::string>* response_headers)
    {
        if (session.is_stdio()) {
            return post_stdio(session, payload, body, http_code);
        }
        const HttpPostOptions opts {
            .max_redirs       = 0,
            .response_headers = response_headers,
        };
        return http_post(session.endpoint.url,
            post_headers(session, negotiated), payload,
            session.endpoint.timeout_secs, body, &http_code, opts);
    }

    // Fire-and-forget notification: stdio writes without waiting; HTTP
    // needs the round trip.
    Status notify(
        McpSession& session, const std::string& payload, long& http_code)
    {
        if (session.is_stdio()) {
            http_code = 200;
            return session.stdio.write(payload);
        }
        std::string body;
        return post(session, payload, true, http_code, body, nullptr);
    }

    std::string http_status_detail(long code, std::string_view phase)
    {
        return "HTTP " + std::to_string(code) + " " + std::string(phase);
    }

    // One sessioned RPC exchange with the 404 session-expiry retry. The
    // envelope needs optional `result` and optional<McpRpcError> `error`.
    template <typename Response>
    Status mcp_sessioned_rpc(McpSession& session, std::string_view method,
        const JsonValue& params, Response& response, std::string& detail)
    {
        for (int attempt = 0; attempt < 2; ++attempt) {
            long code = 0;
            std::string body;
            if (post(session,
                    mcp_rpc_request(++session.next_id, method, params), true,
                    code, body, nullptr)
                != Status::OK) {
                detail = std::string(method) + " request failed";
                return Status::NETWORK_ERROR;
            }
            // Spec: HTTP 404 on a sessioned request means the server
            // dropped the session; start a new one and retry once.
            if (code == 404 && attempt == 0 && !session.session_id.empty()) {
                const Status reinit = mcp_initialize(session, detail);
                if (reinit != Status::OK) {
                    return reinit;
                }
                continue;
            }
            if (!http_ok(code)) {
                detail = http_status_detail(code, method);
                return Status::API_ERROR;
            }
            std::string message;
            if (!mcp_find_rpc_response(body, message)) {
                detail = std::string(method)
                    + " response carried no JSON-RPC message";
                return Status::JSON_ERROR;
            }
            if (const glz::error_ctx error
                = json_parse_checked(message, response)) {
                detail = json_parse_error(message, error);
                return Status::JSON_ERROR;
            }
            if (response.error) {
                detail = "rpc error " + std::to_string(response.error->code)
                    + ": " + response.error->message;
                return Status::API_ERROR;
            }
            if (!response.result) {
                detail = std::string(method) + " returned no result";
                return Status::JSON_ERROR;
            }
            return Status::OK;
        }
        detail = "session expired twice";
        return Status::API_ERROR;
    }

} // namespace

std::string mcp_rpc_request(
    std::uint64_t id, std::string_view method, const JsonValue& params)
{
    JsonValue request;
    request["jsonrpc"] = "2.0";
    request["id"]      = static_cast<double>(id);
    request["method"]  = method;
    request["params"]  = params;
    return json_dump(request);
}

bool mcp_version_supported(std::string_view version)
{
    return std::find(
               std::begin(KNOWN_VERSIONS), std::end(KNOWN_VERSIONS), version)
        != std::end(KNOWN_VERSIONS);
}

bool mcp_find_rpc_response(std::string_view body, std::string& out)
{
    if (is_rpc_message(parse_json(body))) {
        out = std::string(trim(body));
        return true;
    }
    for (const std::string_view line : split_lines(body)) {
        const std::string_view trimmed = trim(line);
        if (!trimmed.starts_with("data:")) {
            continue;
        }
        const std::string_view data = trim(trimmed.substr(5));
        if (data.empty()) {
            continue;
        }
        if (is_rpc_message(parse_json(data))) {
            out = std::string(data);
            return true;
        }
    }
    return false;
}

std::string mcp_session_id(const std::vector<std::string>& headers)
{
    constexpr std::string_view SESSION_KEY = "mcp-session-id";
    for (const std::string& header : headers) {
        const std::size_t colon = header.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const std::string_view name(header.data(), colon);
        if (name.size() != SESSION_KEY.size()) {
            continue;
        }
        if (!std::equal(name.begin(), name.end(), SESSION_KEY.begin(),
                [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a))
                        == std::tolower(static_cast<unsigned char>(b));
                })) {
            continue;
        }
        const std::string value(trim(std::string_view(
            header.data() + colon + 1, header.size() - colon - 1)));
        if (!value.empty()) {
            return value;
        }
    }
    return "";
}

Status mcp_initialize(McpSession& session, std::string& detail)
{
    session.session_id.clear();
    session.protocol_version = std::string(MCP_PROTOCOL_VERSION);
    session.initialized      = false;

    JsonValue client_info;
    client_info["name"]    = "imza";
    client_info["version"] = IMZA_VERSION;

    JsonValue params;
    params["protocolVersion"] = MCP_PROTOCOL_VERSION;
    params["capabilities"]    = JsonValue(JsonValue::object_t { });
    params["clientInfo"]      = std::move(client_info);

    long code = 0;
    std::string body;
    std::vector<std::string> response_headers;
    if (post(session, mcp_rpc_request(++session.next_id, "initialize", params),
            false, code, body, &response_headers)
        != Status::OK) {
        detail = "initialize request failed";
        return Status::NETWORK_ERROR;
    }
    if (!http_ok(code)) {
        detail = http_status_detail(code, "during initialize");
        return Status::API_ERROR;
    }
    if (!session.is_stdio()) {
        session.session_id = mcp_session_id(response_headers);
    }

    std::string message;
    if (!mcp_find_rpc_response(body, message)) {
        detail = "initialize response carried no JSON-RPC message";
        return Status::JSON_ERROR;
    }
    McpInitializeResponse response;
    if (const glz::error_ctx error = json_parse_checked(message, response)) {
        detail = json_parse_error(message, error);
        return Status::JSON_ERROR;
    }
    if (!response.result) {
        detail = response.error
            ? "initialize rejected: " + response.error->message
            : "malformed initialize response";
        return Status::API_ERROR;
    }
    const std::string offered = response.result->protocol_version;
    if (!mcp_version_supported(offered)) {
        detail = "unsupported protocol version: " + offered;
        return Status::API_ERROR;
    }
    if (!offered.empty()) {
        session.protocol_version = offered;
    }

    JsonValue notification;
    notification["jsonrpc"] = "2.0";
    notification["method"]  = "notifications/initialized";
    long notify_code        = 0;
    if (notify(session, json_dump(notification), notify_code) != Status::OK) {
        detail = "initialized notification request failed";
        return Status::NETWORK_ERROR;
    }
    if (!http_ok(notify_code)) {
        detail = http_status_detail(
            notify_code, "for the initialized notification");
        return Status::API_ERROR;
    }
    session.initialized = true;
    return Status::OK;
}

Status mcp_call_tool(McpSession& session, const std::string& name,
    const JsonValue& arguments, McpToolCallResult& out, std::string& detail)
{
    if (!session.initialized) {
        detail = "session not initialized";
        return Status::CONFIG_ERROR;
    }
    JsonValue params;
    params["name"]      = name;
    params["arguments"] = arguments;

    McpCallToolResponse response;
    const Status st
        = mcp_sessioned_rpc(session, "tools/call", params, response, detail);
    if (st != Status::OK) {
        return st;
    }
    out = std::move(*response.result);
    return Status::OK;
}

Status mcp_list_tools(McpSession& session, std::vector<McpToolDefinition>& out,
    std::string& detail)
{
    if (!session.initialized) {
        detail = "session not initialized";
        return Status::CONFIG_ERROR;
    }
    std::optional<std::string> cursor;
    // Upper bound on pages: a server that keeps returning cursors must not
    // spin this client forever.
    for (int page = 0; page < 100; ++page) {
        JsonValue params;
        if (cursor) {
            params["cursor"] = *cursor;
        }
        McpListToolsResponse response;
        const Status st = mcp_sessioned_rpc(
            session, "tools/list", params, response, detail);
        if (st != Status::OK) {
            return st;
        }
        if (response.result->tools) {
            for (McpToolDefinition& tool : *response.result->tools) {
                out.push_back(std::move(tool));
            }
        }
        if (!response.result->next_cursor
            || response.result->next_cursor->empty()) {
            return Status::OK;
        }
        cursor = std::move(*response.result->next_cursor);
    }
    detail = "tools/list did not terminate after 100 pages";
    return Status::API_ERROR;
}

Status mcp_send_result(
    McpSession& session, const JsonValue& id, const JsonValue& result)
{
    if (!session.initialized) {
        return Status::CONFIG_ERROR;
    }
    JsonValue response;
    response["jsonrpc"] = "2.0";
    response["id"]      = id;
    response["result"]  = result;
    long code           = 0;
    if (notify(session, json_dump(response), code) != Status::OK) {
        return Status::NETWORK_ERROR;
    }
    return http_ok(code) ? Status::OK : Status::API_ERROR;
}

void mcp_end_session(McpSession& session)
{
    if (session.is_stdio()) {
        session.stdio.terminate();
        session.initialized = false;
        return;
    }
    if (!session.session_id.empty()) {
        std::vector<std::string> headers = auth_and_static_headers(session);
        headers.push_back("MCP-Session-Id: " + session.session_id);
        headers.push_back("MCP-Protocol-Version: " + session.protocol_version);
        long code = 0;
        std::string body;
        http_delete(session.endpoint.url, headers,
            session.endpoint.timeout_secs, &code);
    }
    session.session_id.clear();
    session.initialized = false;
}

std::string mcp_first_text(const McpToolCallResult& result)
{
    if (!result.content) {
        return "";
    }
    for (const McpContentItem& item : *result.content) {
        if (item.text && !trim(*item.text).empty()) {
            return *item.text;
        }
    }
    return "";
}

} // namespace imza
