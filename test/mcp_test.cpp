#include <doctest/doctest.h>

#include "loopback_mcp_server.h"
#include "network/json.h"
#include "network/json_io.h"
#include "network/mcp.h"

#include <string>

TEST_CASE("mcp_rpc_request produces the JSON-RPC 2.0 envelope")
{
    imza::JsonValue params;
    params["protocolVersion"] = "2025-11-25";
    params["capabilities"]    = imza::JsonValue(imza::JsonValue::object_t { });
    params["clientInfo"]["name"] = "imza";

    const imza::JsonValue wire
        = imza::parse_json(imza::mcp_rpc_request(7, "initialize", params));
    REQUIRE(wire.is_object());
    const imza::JsonValue* version = imza::find_member(wire, "jsonrpc");
    const imza::JsonValue* id      = imza::find_member(wire, "id");
    const imza::JsonValue* method  = imza::find_member(wire, "method");
    const imza::JsonValue* inner   = imza::find_member(wire, "params");
    REQUIRE(version != nullptr);
    REQUIRE(id != nullptr);
    REQUIRE(method != nullptr);
    REQUIRE(inner != nullptr);
    CHECK(version->as<std::string>() == "2.0");
    CHECK(id->as<double>() == doctest::Approx(7));
    CHECK(method->as<std::string>() == "initialize");
    const imza::JsonValue* requested
        = imza::find_member(*inner, "protocolVersion");
    const imza::JsonValue* capabilities
        = imza::find_member(*inner, "capabilities");
    REQUIRE(requested != nullptr);
    REQUIRE(capabilities != nullptr);
    CHECK(requested->as<std::string>() == "2025-11-25");
    CHECK(capabilities->is_object());
}

TEST_CASE("mcp_version_supported accepts exactly the known versions")
{
    CHECK(imza::mcp_version_supported("2025-03-26"));
    CHECK(imza::mcp_version_supported("2025-06-18"));
    CHECK(imza::mcp_version_supported("2025-11-25"));
    CHECK_FALSE(imza::mcp_version_supported("2024-11-05"));
    CHECK_FALSE(imza::mcp_version_supported("1.0"));
    CHECK_FALSE(imza::mcp_version_supported("2026-07-28"));
    CHECK_FALSE(imza::mcp_version_supported(""));
}

TEST_CASE("mcp_find_rpc_response reads JSON bodies and SSE data lines")
{
    std::string message;
    REQUIRE(imza::mcp_find_rpc_response(
        R"({"jsonrpc":"2.0","id":1,"result":{"content":[]}})", message));
    CHECK(message.find("\"result\"") != std::string::npos);

    REQUIRE(imza::mcp_find_rpc_response(
        "event: message\n"
        "data: {\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"content\":[]}}\n"
        "\n",
        message));
    CHECK(message.starts_with("{\"jsonrpc\""));

    REQUIRE(imza::mcp_find_rpc_response(
        R"({"jsonrpc":"2.0","id":3,"error":{"code":-32600}})", message));

    CHECK_FALSE(imza::mcp_find_rpc_response(
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})", message));
    CHECK_FALSE(imza::mcp_find_rpc_response("totally not json", message));
    CHECK_FALSE(imza::mcp_find_rpc_response("data: not json\n\n", message));
    CHECK_FALSE(imza::mcp_find_rpc_response("data:\ndata:   \n\n", message));
}

TEST_CASE("mcp_session_id reads the header case-insensitively")
{
    CHECK(imza::mcp_session_id({ "Content-Type: application/json" }) == "");
    CHECK(imza::mcp_session_id({ "MCP-Session-Id: abc123" }) == "abc123");
    CHECK(imza::mcp_session_id({ "mcp-session-id:  xyz " }) == "xyz");
    CHECK(imza::mcp_session_id(
              { "Content-Type: text/event-stream", "Mcp-Session-Id: s1" })
        == "s1");
}

TEST_CASE("mcp_first_text picks the first non-empty text block")
{
    imza::McpContentItem empty;
    empty.text = "";
    imza::McpContentItem first;
    first.type = "text";
    first.text = "first";
    imza::McpContentItem second;
    second.text = "second";

    imza::McpToolCallResult result;
    result.content = { empty, first, second };
    CHECK(imza::mcp_first_text(result) == "first");
    result.content = { empty };
    CHECK(imza::mcp_first_text(result) == "");
    result.content.reset();
    CHECK(imza::mcp_first_text(result) == "");
}

// Loopback server plus the session scaffolding every protocol test
// needs: allow loopback, start, connect, initialize. Server knobs are set
// on `server` before calling start_initialized().
struct LoopbackSession {
    imza::test::LoopbackMcpServer server;
    imza::McpSession session;
    std::string detail;

    bool start_initialized()
    {
        imza::test::allow_loopback_direct();
        if (!server.start()) {
            return false;
        }
        session = imza::test::session_for(server);
        return imza::mcp_initialize(session, detail) == imza::Status::OK;
    }
};

TEST_CASE("mcp session runs the full lifecycle against a loopback server")
{
    LoopbackSession fx;
    REQUIRE(fx.start_initialized());
    imza::McpSession& session = fx.session;
    std::string& detail       = fx.detail;
    CHECK(detail.empty());
    CHECK(session.session_id == "sess-a");
    CHECK(session.protocol_version == "2025-11-25");
    CHECK(session.initialized);

    imza::JsonValue arguments;
    arguments["query"] = "anything";
    imza::McpToolCallResult result;
    REQUIRE(imza::mcp_call_tool(session, "echo", arguments, result, detail)
        == imza::Status::OK);
    CHECK(imza::mcp_first_text(result) == "loopback ok");
    CHECK_FALSE(result.is_error.value_or(false));

    imza::mcp_end_session(session);
    CHECK_FALSE(session.initialized);
    CHECK(session.session_id.empty());

    fx.server.stop();
    CHECK(fx.server.protocol_header_ok.load());
    CHECK(fx.server.session_echo_ok.load());
    CHECK(fx.server.saw_delete.load());
}

TEST_CASE("mcp list tools paginates across cursor pages")
{
    LoopbackSession fx;
    REQUIRE(fx.start_initialized());
    imza::McpSession& session = fx.session;
    std::string& detail       = fx.detail;

    std::vector<imza::McpToolDefinition> tools;
    REQUIRE(imza::mcp_list_tools(session, tools, detail) == imza::Status::OK);
    REQUIRE(tools.size() == 3);
    CHECK(tools[0].name == "echo");
    CHECK(tools[0].description.value_or("") == "Echo back");
    REQUIRE(tools[0].input_schema.has_value());
    CHECK(imza::find_member(*tools[0].input_schema, "type") != nullptr);
    CHECK(tools[1].name == "ping");
    CHECK(tools[2].name == "third");
    CHECK(tools[2].title.value_or("") == "Third");
    CHECK_FALSE(tools[2].input_schema.has_value());

    fx.server.stop();
}

TEST_CASE("mcp list tools refuses an uninitialized session")
{
    imza::McpSession session;
    std::string detail;
    std::vector<imza::McpToolDefinition> tools;
    CHECK(imza::mcp_list_tools(session, tools, detail)
        == imza::Status::CONFIG_ERROR);
    CHECK(detail == "session not initialized");
}

TEST_CASE("mcp call tool re-initializes once when the session expires")
{
    LoopbackSession fx;
    fx.server.expire_first_call = true;
    fx.server.call_text         = "recovered";
    REQUIRE(fx.start_initialized());
    imza::McpSession& session = fx.session;
    std::string& detail       = fx.detail;

    imza::JsonValue arguments;
    imza::McpToolCallResult result;
    REQUIRE(imza::mcp_call_tool(session, "echo", arguments, result, detail)
        == imza::Status::OK);
    CHECK(detail.empty());
    CHECK(imza::mcp_first_text(result) == "recovered");
    CHECK(session.session_id == "sess-b");

    fx.server.stop();
    CHECK(fx.server.session_echo_ok.load());
}

TEST_CASE("mcp initialize accepts a server counter-offered version")
{
    LoopbackSession fx;
    fx.server.init_version = "2025-06-18";
    REQUIRE(fx.start_initialized());
    imza::McpSession& session = fx.session;
    std::string& detail       = fx.detail;
    CHECK(session.protocol_version == "2025-06-18");

    imza::JsonValue arguments;
    imza::McpToolCallResult result;
    REQUIRE(imza::mcp_call_tool(session, "echo", arguments, result, detail)
        == imza::Status::OK);

    fx.server.stop();
    CHECK(fx.server.protocol_header_ok.load());
}

TEST_CASE("mcp initialize fails closed on an unsupported version")
{
    imza::test::allow_loopback_direct();
    imza::test::LoopbackMcpServer server;
    server.init_version = "1.0.0";
    REQUIRE(server.start());

    imza::McpSession session = imza::test::session_for(server);
    std::string detail;
    CHECK(imza::mcp_initialize(session, detail) == imza::Status::API_ERROR);
    CHECK(detail.find("unsupported protocol version") != std::string::npos);
    CHECK_FALSE(session.initialized);

    server.stop();
}

TEST_CASE("mcp call tool reports rpc protocol errors through detail")
{
    LoopbackSession fx;
    fx.server.rpc_error_on_call = true;
    REQUIRE(fx.start_initialized());
    imza::McpSession& session = fx.session;
    std::string& detail       = fx.detail;

    imza::JsonValue arguments;
    imza::McpToolCallResult result;
    CHECK(imza::mcp_call_tool(session, "echo", arguments, result, detail)
        == imza::Status::API_ERROR);
    CHECK(detail.find("Unknown tool: echo") != std::string::npos);

    fx.server.stop();
}

TEST_CASE("mcp tool results carry execution errors and structured content")
{
    LoopbackSession fx;
    fx.server.call_text       = "boom";
    fx.server.call_is_error   = true;
    fx.server.call_structured = true;
    REQUIRE(fx.start_initialized());
    imza::McpSession& session = fx.session;
    std::string& detail       = fx.detail;

    imza::JsonValue arguments;
    imza::McpToolCallResult result;
    // Tool execution errors are valid results (isError), not rpc failures.
    REQUIRE(imza::mcp_call_tool(session, "echo", arguments, result, detail)
        == imza::Status::OK);
    CHECK(imza::mcp_first_text(result) == "boom");
    REQUIRE(result.is_error.has_value());
    CHECK(*result.is_error);
    REQUIRE(result.structured_content.has_value());
    const imza::JsonValue* answer
        = imza::find_member(*result.structured_content, "answer");
    REQUIRE(answer != nullptr);
    CHECK(answer->as<double>() == doctest::Approx(42));

    fx.server.stop();
}
