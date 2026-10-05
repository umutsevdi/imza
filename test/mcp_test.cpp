#include <doctest/doctest.h>

#include "common/util.h"
#include "network/json.h"
#include "network/json_io.h"
#include "network/mcp.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <string_view>
#include <thread>

namespace {

// Scripted MCP server for one scenario: exactly one HTTP request per
// connection, each answered with "Connection: close". The knobs are set
// before start(); the atomic flags record facts the test asserts on after
// stop().
class LoopbackMcpServer {
public:
    std::string init_version = "2025-11-25";
    std::string call_text    = "loopback ok";
    bool expire_first_call   = false;
    bool rpc_error_on_call   = false;
    bool call_is_error       = false;
    bool call_structured     = false;

    std::atomic<bool> session_echo_ok { false };
    std::atomic<bool> protocol_header_ok { false };
    std::atomic<bool> saw_delete { false };

    ~LoopbackMcpServer() { stop(); }

    bool start()
    {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            return false;
        }
        int reuse = 1;
        ::setsockopt(
            listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in address { };
        address.sin_family      = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port        = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address),
                sizeof(address))
                < 0
            || ::listen(listen_fd_, 4) < 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        sockaddr_in bound { };
        socklen_t length = sizeof(bound);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &length);
        url_ = "http://127.0.0.1:" + std::to_string(ntohs(bound.sin_port))
            + "/mcp";
        thread_ = std::thread([this] { serve(); });
        return true;
    }

    const std::string& url() const { return url_; }

    void stop()
    {
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
    }

private:
    struct Request {
        std::string method;
        std::string headers;
        std::string lower_headers;
        std::string body;
    };

    void serve()
    {
        for (;;) {
            const int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) {
                return; // unblocked by stop()'s shutdown
            }
            const timeval timeout { 5, 0 };
            ::setsockopt(
                fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            handle(fd);
            ::close(fd);
        }
    }

    static bool read_request(int fd, Request& request)
    {
        std::string buffer;
        char chunk[4096];
        std::size_t header_end = std::string::npos;
        while ((header_end = buffer.find("\r\n\r\n")) == std::string::npos) {
            const ssize_t received = ::recv(fd, chunk, sizeof(chunk), 0);
            if (received <= 0) {
                return false;
            }
            buffer.append(chunk, static_cast<std::size_t>(received));
            if (buffer.size() > (1u << 20)) {
                return false;
            }
        }
        const std::size_t line_end = buffer.find("\r\n");
        const std::size_t space    = buffer.find(' ');
        if (line_end == std::string::npos || space == std::string::npos
            || space > line_end) {
            return false;
        }
        request.method  = buffer.substr(0, space);
        request.headers = buffer.substr(0, header_end);
        for (const char c : request.headers) {
            request.lower_headers += static_cast<char>(
                std::tolower(static_cast<unsigned char>(c)));
        }
        std::size_t content_length = 0;
        if (const std::size_t at
            = request.lower_headers.find("content-length:");
            at != std::string::npos) {
            content_length = std::strtoul(
                request.lower_headers.c_str() + at + 15, nullptr, 10);
        }
        request.body = buffer.substr(header_end + 4);
        while (request.body.size() < content_length) {
            const ssize_t received = ::recv(fd, chunk, sizeof(chunk), 0);
            if (received <= 0) {
                return false;
            }
            request.body.append(chunk, static_cast<std::size_t>(received));
        }
        return true;
    }

    void handle(int fd)
    {
        Request request;
        if (!read_request(fd, request)) {
            return;
        }
        if (request.method == "DELETE") {
            saw_delete = true;
            respond(fd, "HTTP/1.1 200 OK", { }, "");
            return;
        }
        if (request.body.find("\"method\":\"initialize\"")
            != std::string::npos) {
            ++init_count_;
            last_session_ = init_count_ == 1 ? "sess-a" : "sess-b";
            const std::string body
                = R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":")"
                + init_version
                + R"(","serverInfo":{"name":"loopback","version":"1.0"},)"
                  R"("instructions":"be polite"}})";
            respond(fd, "HTTP/1.1 200 OK",
                { "MCP-Session-Id: " + last_session_,
                    "Content-Type: application/json" },
                body);
            return;
        }
        if (request.body.find("\"method\":\"notifications/initialized\"")
            != std::string::npos) {
            respond(fd, "HTTP/1.1 202 Accepted", { }, "");
            return;
        }
        if (request.body.find("\"method\":\"tools/call\"")
            != std::string::npos) {
            if (expire_first_call && !expired_) {
                expired_ = true;
                respond(fd, "HTTP/1.1 404 Not Found", { }, "");
                return;
            }
            protocol_header_ok = request.lower_headers.find(
                                     "mcp-protocol-version: " + init_version)
                != std::string::npos;
            session_echo_ok
                = request.lower_headers.find("mcp-session-id: " + last_session_)
                != std::string::npos;
            if (rpc_error_on_call) {
                respond(fd, "HTTP/1.1 200 OK",
                    { "Content-Type: application/json" },
                    R"({"jsonrpc":"2.0","id":9,)"
                    R"("error":{"code":-32602,)"
                    R"("message":"Unknown tool: echo"}})");
                return;
            }
            std::string payload
                = R"({"jsonrpc":"2.0","id":9,"result":{"content":)"
                  R"([{"type":"text","text":")"
                + call_text + R"("}])";
            if (call_is_error) {
                payload += R"(,"isError":true)";
            }
            if (call_structured) {
                payload += R"(,"structuredContent":{"answer":42})";
            }
            payload += "}}";
            respond(fd, "HTTP/1.1 200 OK",
                { "Content-Type: text/event-stream" },
                "event: message\ndata: " + payload + "\n\n");
            return;
        }
        respond(fd, "HTTP/1.1 400 Bad Request", { }, "");
    }

    static void respond(int fd, const std::string& status,
        std::initializer_list<std::string> headers, const std::string& body)
    {
        std::string wire = status + "\r\nConnection: close\r\n";
        for (const std::string& header : headers) {
            wire += header + "\r\n";
        }
        wire += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
        wire += body;
        std::size_t sent = 0;
        while (sent < wire.size()) {
            const ssize_t sent_now
                = ::send(fd, wire.data() + sent, wire.size() - sent, 0);
            if (sent_now <= 0) {
                return;
            }
            sent += static_cast<std::size_t>(sent_now);
        }
    }

    int listen_fd_ = -1;
    std::thread thread_;
    std::string url_;
    std::string last_session_ = "sess-a";
    int init_count_           = 0;
    bool expired_             = false;
};

void allow_loopback_direct()
{
    // Bypass any ambient proxy settings for the local fixture server.
    ::setenv("NO_PROXY", "127.0.0.1,localhost", 1);
    ::setenv("no_proxy", "127.0.0.1,localhost", 1);
}

imza::McpSession session_for(const LoopbackMcpServer& server)
{
    imza::McpSession session;
    session.endpoint.url          = server.url();
    session.endpoint.timeout_secs = 5;
    return session;
}

} // namespace

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

TEST_CASE("mcp session runs the full lifecycle against a loopback server")
{
    allow_loopback_direct();
    LoopbackMcpServer server;
    REQUIRE(server.start());

    imza::McpSession session = session_for(server);
    std::string detail;
    REQUIRE(imza::mcp_initialize(session, detail) == imza::Status::OK);
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

    server.stop();
    CHECK(server.protocol_header_ok.load());
    CHECK(server.session_echo_ok.load());
    CHECK(server.saw_delete.load());
}

TEST_CASE("mcp call tool re-initializes once when the session expires")
{
    allow_loopback_direct();
    LoopbackMcpServer server;
    server.expire_first_call = true;
    server.call_text         = "recovered";
    REQUIRE(server.start());

    imza::McpSession session = session_for(server);
    std::string detail;
    REQUIRE(imza::mcp_initialize(session, detail) == imza::Status::OK);

    imza::JsonValue arguments;
    imza::McpToolCallResult result;
    REQUIRE(imza::mcp_call_tool(session, "echo", arguments, result, detail)
        == imza::Status::OK);
    CHECK(detail.empty());
    CHECK(imza::mcp_first_text(result) == "recovered");
    CHECK(session.session_id == "sess-b");

    server.stop();
    CHECK(server.session_echo_ok.load());
}

TEST_CASE("mcp initialize accepts a server counter-offered version")
{
    allow_loopback_direct();
    LoopbackMcpServer server;
    server.init_version = "2025-06-18";
    REQUIRE(server.start());

    imza::McpSession session = session_for(server);
    std::string detail;
    REQUIRE(imza::mcp_initialize(session, detail) == imza::Status::OK);
    CHECK(session.protocol_version == "2025-06-18");

    imza::JsonValue arguments;
    imza::McpToolCallResult result;
    REQUIRE(imza::mcp_call_tool(session, "echo", arguments, result, detail)
        == imza::Status::OK);

    server.stop();
    CHECK(server.protocol_header_ok.load());
}

TEST_CASE("mcp initialize fails closed on an unsupported version")
{
    allow_loopback_direct();
    LoopbackMcpServer server;
    server.init_version = "1.0.0";
    REQUIRE(server.start());

    imza::McpSession session = session_for(server);
    std::string detail;
    CHECK(imza::mcp_initialize(session, detail) == imza::Status::API_ERROR);
    CHECK(detail.find("unsupported protocol version") != std::string::npos);
    CHECK_FALSE(session.initialized);

    server.stop();
}

TEST_CASE("mcp call tool reports rpc protocol errors through detail")
{
    allow_loopback_direct();
    LoopbackMcpServer server;
    server.rpc_error_on_call = true;
    REQUIRE(server.start());

    imza::McpSession session = session_for(server);
    std::string detail;
    REQUIRE(imza::mcp_initialize(session, detail) == imza::Status::OK);

    imza::JsonValue arguments;
    imza::McpToolCallResult result;
    CHECK(imza::mcp_call_tool(session, "echo", arguments, result, detail)
        == imza::Status::API_ERROR);
    CHECK(detail.find("Unknown tool: echo") != std::string::npos);

    server.stop();
}

TEST_CASE("mcp tool results carry execution errors and structured content")
{
    allow_loopback_direct();
    LoopbackMcpServer server;
    server.call_text       = "boom";
    server.call_is_error   = true;
    server.call_structured = true;
    REQUIRE(server.start());

    imza::McpSession session = session_for(server);
    std::string detail;
    REQUIRE(imza::mcp_initialize(session, detail) == imza::Status::OK);

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

    server.stop();
}
