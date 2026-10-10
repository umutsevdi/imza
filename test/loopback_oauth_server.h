#pragma once

// Scripted OAuth authorization server + MCP resource server shared by the
// mcp_oauth and mcp_manager tests: exactly one HTTP request per
// connection, each answered with "Connection: close". Serves RFC 8414
// metadata, RFC 7591 registration, an /authorize redirect, RFC 6749 token
// grants, and an MCP endpoint gated on the bearer token it issued. Knobs
// are set before start(); the atomic flags record facts the test asserts
// on after stop().

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <string>
#include <thread>

namespace imza::test {

class LoopbackOauthServer {
public:
    // MCP endpoint auth
    bool require_bearer     = true;  // /mcp 401s without the issued token
    bool no_metadata_header = false; // 401 omits WWW-Authenticate
    // Authorization server
    bool no_registration    = false; // metadata omits registration_endpoint
    bool registration_fails = false; // /register answers 500
    bool deny_authorization = false; // /authorize redirects with an error
    bool invalid_grant      = false; // /token answers invalid_grant
    std::string issued_client_id = "client-test";
    std::string issued_code      = "auth-code-1";
    std::string issued_refresh   = "refresh-ok";
    long issued_expires_in       = 3600;

    std::atomic<int> exchange_count { 0 };
    std::atomic<int> refresh_count { 0 };
    std::atomic<int> unauthorized_mcp { 0 };
    std::atomic<bool> saw_code_verifier { false };
    std::atomic<bool> saw_redirect_uri { false };
    std::atomic<bool> saw_register { false };

    ~LoopbackOauthServer() { stop(); }

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
            || ::listen(listen_fd_, 8) < 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        sockaddr_in bound { };
        socklen_t length = sizeof(bound);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &length);
        base_   = "http://127.0.0.1:" + std::to_string(ntohs(bound.sin_port));
        thread_ = std::thread([this] { serve(); });
        return true;
    }

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

    const std::string& base_url() const { return base_; }
    std::string mcp_url() const { return base_ + "/mcp"; }
    // The token the MCP endpoint currently accepts; each grant rotates it.
    std::string current_access_token() const
    {
        return "tok-" + std::to_string(generation_.load());
    }
    // Invalidates the accepted token without issuing a new one (a revoked
    // or expired access token with a still-valid refresh token).
    void revoke_access_token() { generation_.fetch_add(1); }

private:
    struct Request {
        std::string method;
        std::string path;  // without the query string
        std::string query; // without the leading '?'
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
        const std::size_t second = buffer.find(' ', space + 1);
        if (second == std::string::npos || second > line_end) {
            return false;
        }
        request.method           = buffer.substr(0, space);
        const std::string target = buffer.substr(space + 1, second - space - 1);
        const auto query_at      = target.find('?');
        request.path             = target.substr(0, query_at);
        request.query
            = query_at == std::string::npos ? "" : target.substr(query_at + 1);
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

    static std::string percent_decode(const std::string& value)
    {
        std::string out;
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '%' && i + 2 < value.size()) {
                const auto hex = [](char c) -> int {
                    if (c >= '0' && c <= '9') {
                        return c - '0';
                    }
                    if (c >= 'a' && c <= 'f') {
                        return c - 'a' + 10;
                    }
                    if (c >= 'A' && c <= 'F') {
                        return c - 'A' + 10;
                    }
                    return -1;
                };
                const int high = hex(value[i + 1]);
                const int low  = hex(value[i + 2]);
                if (high >= 0 && low >= 0) {
                    out.push_back(static_cast<char>(high * 16 + low));
                    i += 2;
                    continue;
                }
            }
            out.push_back(value[i]);
        }
        return out;
    }

    // Decoded query/form pairs (both use & and = separators).
    static std::map<std::string, std::string> parse_pairs(
        const std::string& raw)
    {
        std::map<std::string, std::string> out;
        std::size_t at = 0;
        while (at <= raw.size()) {
            const auto ampersand   = raw.find('&', at);
            const std::string pair = raw.substr(at,
                ampersand == std::string::npos ? std::string::npos
                                               : ampersand - at);
            const auto equals      = pair.find('=');
            if (equals != std::string::npos) {
                out[percent_decode(pair.substr(0, equals))]
                    = percent_decode(pair.substr(equals + 1));
            }
            if (ampersand == std::string::npos) {
                break;
            }
            at = ampersand + 1;
        }
        return out;
    }

    void handle(int fd)
    {
        Request request;
        if (!read_request(fd, request)) {
            return;
        }
        if (request.path == "/mcp") {
            handle_mcp(fd, request);
            return;
        }
        if (request.path == "/.well-known/oauth-protected-resource") {
            respond(fd, "HTTP/1.1 200 OK", { "Content-Type: application/json" },
                R"({"resource":")" + mcp_url()
                    + R"(","authorization_servers":[")" + base_ + R"("]})");
            return;
        }
        if (request.path == "/.well-known/oauth-authorization-server") {
            std::string body = R"({"issuer":")" + base_
                + R"(","authorization_endpoint":")" + base_
                + R"(/authorize","token_endpoint":")" + base_ + R"(/token")";
            if (!no_registration) {
                body += R"(,"registration_endpoint":")" + base_
                    + R"(/register")";
            }
            body += R"(,"scopes_supported":["read","write"]})";
            respond(fd, "HTTP/1.1 200 OK", { "Content-Type: application/json" },
                body);
            return;
        }
        if (request.path == "/register") {
            saw_register = true;
            if (registration_fails) {
                respond(fd, "HTTP/1.1 500 Server Error", { }, "");
                return;
            }
            respond(fd, "HTTP/1.1 201 Created",
                { "Content-Type: application/json" },
                R"({"client_id":")" + issued_client_id + R"("})");
            return;
        }
        if (request.path == "/authorize") {
            const auto params   = parse_pairs(request.query);
            const auto state    = params.find("state");
            const auto redirect = params.find("redirect_uri");
            if (redirect == params.end()) {
                respond(fd, "HTTP/1.1 400 Bad Request", { }, "");
                return;
            }
            std::string suffix
                = (state == params.end() ? "" : "&state=" + state->second);
            if (deny_authorization) {
                suffix += "&error=access_denied";
            } else {
                suffix += "&code=" + issued_code;
            }
            respond(fd, "HTTP/1.1 302 Found",
                { "Location: " + redirect->second + "?" + suffix.substr(1) },
                "");
            return;
        }
        if (request.path == "/token") {
            handle_token(fd, parse_pairs(request.body));
            return;
        }
        respond(fd, "HTTP/1.1 404 Not Found", { }, "");
    }

    void handle_mcp(int fd, const Request& request)
    {
        const std::string expected
            = "authorization: bearer " + current_access_token();
        if (require_bearer
            && request.lower_headers.find(expected) == std::string::npos) {
            ++unauthorized_mcp;
            std::string header = "WWW-Authenticate: Bearer ";
            if (!no_metadata_header) {
                header += "resource_metadata=\"" + base_
                    + "/.well-known/oauth-protected-resource\"";
            }
            respond(fd, "HTTP/1.1 401 Unauthorized", { header }, "");
            return;
        }
        if (request.method == "GET" || request.method == "DELETE") {
            respond(fd, "HTTP/1.1 405 Method Not Allowed", { }, "");
            return;
        }
        if (request.body.find("\"method\":\"initialize\"")
            != std::string::npos) {
            respond(fd, "HTTP/1.1 200 OK",
                { "MCP-Session-Id: sess-oauth",
                    "Content-Type: application/json" },
                R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":)"
                R"("2025-11-25","serverInfo":{"name":"oauth-loopback"}}})");
            return;
        }
        if (request.body.find("\"method\":\"tools/list\"")
            != std::string::npos) {
            respond(fd, "HTTP/1.1 200 OK", { "Content-Type: application/json" },
                R"({"jsonrpc":"2.0","id":5,"result":{"tools":[)"
                R"({"name":"echo","description":"Echo",)"
                R"("inputSchema":{"type":"object"}}]}})");
            return;
        }
        if (request.body.find("\"method\":\"tools/call\"")
            != std::string::npos) {
            respond(fd, "HTTP/1.1 200 OK", { "Content-Type: application/json" },
                R"({"jsonrpc":"2.0","id":9,"result":{"content":[)"
                R"({"type":"text","text":"oauth ok"}]}})");
            return;
        }
        respond(fd, "HTTP/1.1 202 Accepted", { }, "");
    }

    void handle_token(int fd, const std::map<std::string, std::string>& form)
    {
        if (invalid_grant) {
            respond(fd, "HTTP/1.1 400 Bad Request",
                { "Content-Type: application/json" },
                R"({"error":"invalid_grant"})");
            return;
        }
        const auto grant = form.find("grant_type");
        if (grant == form.end()) {
            respond(fd, "HTTP/1.1 400 Bad Request", { }, "");
            return;
        }
        if (grant->second == "authorization_code") {
            const auto code     = form.find("code");
            const auto verifier = form.find("code_verifier");
            const auto redirect = form.find("redirect_uri");
            if (code == form.end() || code->second != issued_code) {
                respond(fd, "HTTP/1.1 400 Bad Request",
                    { "Content-Type: application/json" },
                    R"({"error":"invalid_grant"})");
                return;
            }
            saw_code_verifier
                = verifier != form.end() && !verifier->second.empty();
            saw_redirect_uri
                = redirect != form.end() && !redirect->second.empty();
            ++exchange_count;
        } else if (grant->second == "refresh_token") {
            ++refresh_count; // every attempt, successful or not
            const auto token = form.find("refresh_token");
            if (token == form.end() || token->second != issued_refresh) {
                respond(fd, "HTTP/1.1 400 Bad Request",
                    { "Content-Type: application/json" },
                    R"({"error":"invalid_grant"})");
                return;
            }
        } else {
            respond(fd, "HTTP/1.1 400 Bad Request", { }, "");
            return;
        }
        generation_.fetch_add(1);
        respond(fd, "HTTP/1.1 200 OK", { "Content-Type: application/json" },
            R"({"access_token":")" + current_access_token()
                + R"(","refresh_token":")" + issued_refresh
                + R"(","expires_in":)" + std::to_string(issued_expires_in)
                + "}");
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
    std::string base_;
    std::atomic<int> generation_ { 0 };
};

} // namespace imza::test
