#pragma once

// Scripted MCP server shared by the mcp client and manager tests, on the
// shared loopback HTTP core (loopback_http.h). Knobs are set before
// start(); the atomic flags record facts the test asserts on after
// stop().

#include "loopback_http.h"
#include "network/mcp.h"

#include <atomic>
#include <chrono>
#include <string>

namespace imza::test {

class LoopbackMcpServer {
public:
    std::string init_version = "2025-11-25";
    std::string call_text    = "loopback ok";
    bool expire_first_call   = false;
    bool rpc_error_on_call   = false;
    bool call_is_error       = false;
    bool call_structured     = false;
    int fail_handshakes      = 0;     // first N initialize requests answer 500
    bool get_returns_405     = false; // GET answers 405 (no server stream)
    bool get_returns_404     = false; // GET answers 404 (session expired)
    bool push_list_changed   = false; // first GET pushes tools/list_changed

    std::atomic<bool> session_echo_ok { false };
    std::atomic<bool> protocol_header_ok { false };
    std::atomic<bool> saw_delete { false };
    std::atomic<bool> saw_get { false };
    std::atomic<bool> rejected_null_params { false };
    std::atomic<int> initialize_count { 0 };
    // tools/list answers 400 with a JSON-RPC error body (error-detail
    // extraction coverage).
    bool fail_tools_list = false;

    bool start()
    {
        http_.handle = [this](int fd, const LoopbackRequest& request) {
            handle(fd, request);
        };
        return http_.start();
    }

    void stop() { http_.stop(); }

    std::string url() const { return http_.base_url() + "/mcp"; }

private:
    void handle(int fd, const LoopbackRequest& request)
    {
        if (request.method == "DELETE") {
            saw_delete = true;
            LoopbackHttpServer::respond(fd, "HTTP/1.1 200 OK", { }, "");
            return;
        }
        if (request.method == "GET") {
            saw_get = true;
            if (get_returns_405) {
                LoopbackHttpServer::respond(
                    fd, "HTTP/1.1 405 Method Not Allowed", { }, "");
                return;
            }
            if (get_returns_404) {
                LoopbackHttpServer::respond(
                    fd, "HTTP/1.1 404 Not Found", { }, "");
                return;
            }
            if (push_list_changed && !pushed_) {
                pushed_ = true;
                LoopbackHttpServer::respond(fd, "HTTP/1.1 200 OK",
                    { "Content-Type: text/event-stream" },
                    "id: 1\nevent: message\ndata: "
                    "{\"jsonrpc\":\"2.0\",\"method\":"
                    "\"notifications/tools/list_changed\"}\n\n");
                return;
            }
            // Park the stream briefly, then close: the client reconnects.
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            LoopbackHttpServer::respond(fd, "HTTP/1.1 200 OK",
                { "Content-Type: text/event-stream" }, "");
            return;
        }
        if (request.body.find("\"method\":\"initialize\"")
            != std::string::npos) {
            if (failed_count_ < fail_handshakes) {
                ++failed_count_;
                LoopbackHttpServer::respond(
                    fd, "HTTP/1.1 500 Server Error", { }, "");
                return;
            }
            ++init_count_;
            ++initialize_count;
            last_session_ = init_count_ == 1 ? "sess-a" : "sess-b";
            const std::string body
                = R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":")"
                + init_version
                + R"(","serverInfo":{"name":"loopback","version":"1.0"},)"
                  R"("instructions":"be polite"}})";
            LoopbackHttpServer::respond(fd, "HTTP/1.1 200 OK",
                { "MCP-Session-Id: " + last_session_,
                    "Content-Type: application/json" },
                body);
            return;
        }
        if (request.body.find("\"method\":\"notifications/initialized\"")
            != std::string::npos) {
            LoopbackHttpServer::respond(fd, "HTTP/1.1 202 Accepted", { }, "");
            return;
        }
        if (request.body.find("\"method\":\"tools/list\"")
            != std::string::npos) {
            // Strict like real gateways (Cloudflare rejects the envelope
            // outright): "params":null is not a valid JSON-RPC message.
            if (request.body.find("\"params\":null") != std::string::npos) {
                rejected_null_params = true;
                LoopbackHttpServer::respond(fd, "HTTP/1.1 400 Bad Request",
                    { "Content-Type: application/json" },
                    R"({"jsonrpc":"2.0","error":{"code":-32600,)"
                    R"("message":"Bad Request: the request body is not a)"
                    R"( valid JSON-RPC message"},"id":0})");
                return;
            }
            if (fail_tools_list) {
                LoopbackHttpServer::respond(fd, "HTTP/1.1 400 Bad Request",
                    { "Content-Type: application/json" },
                    R"({"jsonrpc":"2.0","id":5,"error":{"code":-32000,)"
                    R"("message":"listing unavailable"}})");
                return;
            }
            // After a pushed tools/list_changed the roster is the new set.
            if (pushed_) {
                LoopbackHttpServer::respond(fd, "HTTP/1.1 200 OK",
                    { "Content-Type: application/json" },
                    R"({"jsonrpc":"2.0","id":7,"result":{"tools":[)"
                    R"({"name":"changed-tool","description":"Updated"})"
                    R"(]}})");
                return;
            }
            // Two pages: two tools + cursor, then one tool without.
            const bool first_page = list_count_ == 0;
            ++list_count_;
            const std::string body = first_page
                ? R"({"jsonrpc":"2.0","id":5,"result":{"tools":[)"
                  R"({"name":"echo","description":"Echo back",)"
                  R"("inputSchema":{"type":"object"}},)"
                  R"({"name":"ping","description":"Ping",)"
                  R"("inputSchema":{"type":"object","properties":)"
                  R"({"host":{"type":"string"}}}}],)"
                  R"("nextCursor":"page-2"}})"
                : R"({"jsonrpc":"2.0","id":6,"result":{"tools":[)"
                  R"({"name":"third","title":"Third",)"
                  R"("description":"Third tool"})"
                  R"(]}})";
            LoopbackHttpServer::respond(fd, "HTTP/1.1 200 OK",
                { "Content-Type: application/json" }, body);
            return;
        }
        if (request.body.find("\"method\":\"tools/call\"")
            != std::string::npos) {
            if (expire_first_call && !expired_) {
                expired_ = true;
                LoopbackHttpServer::respond(
                    fd, "HTTP/1.1 404 Not Found", { }, "");
                return;
            }
            protocol_header_ok = request.lower_headers.find(
                                     "mcp-protocol-version: " + init_version)
                != std::string::npos;
            session_echo_ok
                = request.lower_headers.find("mcp-session-id: " + last_session_)
                != std::string::npos;
            if (rpc_error_on_call) {
                LoopbackHttpServer::respond(fd, "HTTP/1.1 200 OK",
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
            LoopbackHttpServer::respond(fd, "HTTP/1.1 200 OK",
                { "Content-Type: text/event-stream" },
                "event: message\ndata: " + payload + "\n\n");
            return;
        }
        LoopbackHttpServer::respond(fd, "HTTP/1.1 400 Bad Request", { }, "");
    }

    LoopbackHttpServer http_;
    std::string last_session_ = "sess-a";
    int init_count_           = 0;
    int list_count_           = 0;
    int failed_count_         = 0;
    bool pushed_              = false;
    bool expired_             = false;
};

inline McpSession session_for(const LoopbackMcpServer& server)
{
    McpSession session;
    session.endpoint.url          = server.url();
    session.endpoint.timeout_secs = 5;
    return session;
}

} // namespace imza::test
