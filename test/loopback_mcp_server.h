#pragma once

// Scripted MCP server shared by the mcp client and manager tests: exactly
// one HTTP request per connection, each answered with "Connection: close".
// Knobs are set before start(); the atomic flags record facts the test
// asserts on after stop().

#include "network/mcp.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <thread>

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
    bool push_list_changed   = false; // first GET pushes tools/list_changed

    std::atomic<bool> session_echo_ok { false };
    std::atomic<bool> protocol_header_ok { false };
    std::atomic<bool> saw_delete { false };
    std::atomic<bool> saw_get { false };

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
        if (request.method == "GET") {
            saw_get = true;
            if (get_returns_405) {
                respond(fd, "HTTP/1.1 405 Method Not Allowed", { }, "");
                return;
            }
            if (push_list_changed && !pushed_) {
                pushed_ = true;
                respond(fd, "HTTP/1.1 200 OK",
                    { "Content-Type: text/event-stream" },
                    "id: 1\nevent: message\ndata: "
                    "{\"jsonrpc\":\"2.0\",\"method\":"
                    "\"notifications/tools/list_changed\"}\n\n");
                return;
            }
            // Park the stream briefly, then close: the client reconnects.
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            respond(fd, "HTTP/1.1 200 OK",
                { "Content-Type: text/event-stream" }, "");
            return;
        }
        if (request.body.find("\"method\":\"initialize\"")
            != std::string::npos) {
            if (failed_count_ < fail_handshakes) {
                ++failed_count_;
                respond(fd, "HTTP/1.1 500 Server Error", { }, "");
                return;
            }
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
        if (request.body.find("\"method\":\"tools/list\"")
            != std::string::npos) {
            // After a pushed tools/list_changed the roster is the new set.
            if (pushed_) {
                respond(fd, "HTTP/1.1 200 OK",
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
            respond(fd, "HTTP/1.1 200 OK", { "Content-Type: application/json" },
                body);
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
    int list_count_           = 0;
    int failed_count_         = 0;
    bool pushed_              = false;
    bool expired_             = false;
};

inline void allow_loopback_direct()
{
    // Bypass any ambient proxy settings for the local fixture server.
    ::setenv("NO_PROXY", "127.0.0.1,localhost", 1);
    ::setenv("no_proxy", "127.0.0.1,localhost", 1);
}

inline McpSession session_for(const LoopbackMcpServer& server)
{
    McpSession session;
    session.endpoint.url          = server.url();
    session.endpoint.timeout_secs = 5;
    return session;
}

} // namespace imza::test
