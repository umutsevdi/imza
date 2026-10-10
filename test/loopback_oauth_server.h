#pragma once

// Scripted OAuth authorization server + MCP resource server shared by the
// mcp_oauth and mcp_manager tests, on the shared loopback HTTP core
// (loopback_http.h). Serves RFC 8414 metadata, RFC 7591 registration, an
// /authorize redirect, RFC 6749 token grants, and an MCP endpoint gated
// on the bearer token it issued. Knobs are set before start(); the atomic
// flags record facts the test asserts on after stop().

#include "common/util.h"
#include "loopback_http.h"
#include "network/network.h"

#include <atomic>
#include <map>
#include <string>

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

    bool start()
    {
        http_.handle = [this](int fd, const LoopbackRequest& request) {
            handle(fd, request);
        };
        return http_.start();
    }

    void stop() { http_.stop(); }

    const std::string& base_url() const { return http_.base_url(); }
    std::string mcp_url() const { return http_.base_url() + "/mcp"; }
    // The token the MCP endpoint currently accepts; each grant rotates it.
    std::string current_access_token() const
    {
        return "tok-" + std::to_string(generation_.load());
    }
    // Invalidates the accepted token without issuing a new one (a revoked
    // or expired access token with a still-valid refresh token).
    void revoke_access_token() { generation_.fetch_add(1); }

private:
    void handle(int fd, const LoopbackRequest& request)
    {
        if (request.path == "/mcp") {
            handle_mcp(fd, request);
            return;
        }
        if (request.path == "/.well-known/oauth-protected-resource") {
            LoopbackHttpServer::respond(fd, "HTTP/1.1 200 OK",
                { "Content-Type: application/json" },
                R"({"resource":")" + mcp_url()
                    + R"(","authorization_servers":[")" + base_url()
                    + R"("]})");
            return;
        }
        if (request.path == "/.well-known/oauth-authorization-server") {
            std::string body = R"({"issuer":")" + base_url()
                + R"(","authorization_endpoint":")" + base_url()
                + R"(/authorize","token_endpoint":")" + base_url()
                + R"(/token")";
            if (!no_registration) {
                body += R"(,"registration_endpoint":")" + base_url()
                    + R"(/register")";
            }
            body += R"(,"scopes_supported":["read","write"]})";
            LoopbackHttpServer::respond(fd, "HTTP/1.1 200 OK",
                { "Content-Type: application/json" }, body);
            return;
        }
        if (request.path == "/register") {
            saw_register = true;
            if (registration_fails) {
                LoopbackHttpServer::respond(
                    fd, "HTTP/1.1 500 Server Error", { }, "");
                return;
            }
            LoopbackHttpServer::respond(fd, "HTTP/1.1 201 Created",
                { "Content-Type: application/json" },
                R"({"client_id":")" + issued_client_id + R"("})");
            return;
        }
        if (request.path == "/authorize") {
            const auto params   = parse_pairs(request.query);
            const auto state    = params.find("state");
            const auto redirect = params.find("redirect_uri");
            if (redirect == params.end()) {
                LoopbackHttpServer::respond(
                    fd, "HTTP/1.1 400 Bad Request", { }, "");
                return;
            }
            std::string suffix
                = (state == params.end() ? "" : "&state=" + state->second);
            if (deny_authorization) {
                suffix += "&error=access_denied";
            } else {
                suffix += "&code=" + issued_code;
            }
            LoopbackHttpServer::respond(fd, "HTTP/1.1 302 Found",
                { "Location: " + redirect->second + "?" + suffix.substr(1) },
                "");
            return;
        }
        if (request.path == "/token") {
            handle_token(fd, parse_pairs(request.body));
            return;
        }
        LoopbackHttpServer::respond(fd, "HTTP/1.1 404 Not Found", { }, "");
    }

    void handle_mcp(int fd, const LoopbackRequest& request)
    {
        const std::string expected
            = "authorization: bearer " + current_access_token();
        if (require_bearer
            && request.lower_headers.find(expected) == std::string::npos) {
            ++unauthorized_mcp;
            std::string header = "WWW-Authenticate: Bearer ";
            if (!no_metadata_header) {
                header += "resource_metadata=\"" + base_url()
                    + "/.well-known/oauth-protected-resource\"";
            }
            LoopbackHttpServer::respond(
                fd, "HTTP/1.1 401 Unauthorized", { header }, "");
            return;
        }
        if (request.method == "GET" || request.method == "DELETE") {
            LoopbackHttpServer::respond(
                fd, "HTTP/1.1 405 Method Not Allowed", { }, "");
            return;
        }
        if (request.body.find("\"method\":\"initialize\"")
            != std::string::npos) {
            LoopbackHttpServer::respond(fd, "HTTP/1.1 200 OK",
                { "MCP-Session-Id: sess-oauth",
                    "Content-Type: application/json" },
                R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":)"
                R"("2025-11-25","serverInfo":{"name":"oauth-loopback"}}})");
            return;
        }
        if (request.body.find("\"method\":\"tools/list\"")
            != std::string::npos) {
            LoopbackHttpServer::respond(fd, "HTTP/1.1 200 OK",
                { "Content-Type: application/json" },
                R"({"jsonrpc":"2.0","id":5,"result":{"tools":[)"
                R"({"name":"echo","description":"Echo",)"
                R"("inputSchema":{"type":"object"}}]}})");
            return;
        }
        if (request.body.find("\"method\":\"tools/call\"")
            != std::string::npos) {
            LoopbackHttpServer::respond(fd, "HTTP/1.1 200 OK",
                { "Content-Type: application/json" },
                R"({"jsonrpc":"2.0","id":9,"result":{"content":[)"
                R"({"type":"text","text":"oauth ok"}]}})");
            return;
        }
        LoopbackHttpServer::respond(fd, "HTTP/1.1 202 Accepted", { }, "");
    }

    void handle_token(int fd, const std::map<std::string, std::string>& form)
    {
        if (invalid_grant) {
            LoopbackHttpServer::respond(fd, "HTTP/1.1 400 Bad Request",
                { "Content-Type: application/json" },
                R"({"error":"invalid_grant"})");
            return;
        }
        const auto grant = form.find("grant_type");
        if (grant == form.end()) {
            LoopbackHttpServer::respond(
                fd, "HTTP/1.1 400 Bad Request", { }, "");
            return;
        }
        if (grant->second == "authorization_code") {
            const auto code     = form.find("code");
            const auto verifier = form.find("code_verifier");
            const auto redirect = form.find("redirect_uri");
            if (code == form.end() || code->second != issued_code) {
                LoopbackHttpServer::respond(fd, "HTTP/1.1 400 Bad Request",
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
                LoopbackHttpServer::respond(fd, "HTTP/1.1 400 Bad Request",
                    { "Content-Type: application/json" },
                    R"({"error":"invalid_grant"})");
                return;
            }
        } else {
            LoopbackHttpServer::respond(
                fd, "HTTP/1.1 400 Bad Request", { }, "");
            return;
        }
        generation_.fetch_add(1);
        LoopbackHttpServer::respond(fd, "HTTP/1.1 200 OK",
            { "Content-Type: application/json" },
            R"({"access_token":")" + current_access_token()
                + R"(","refresh_token":")" + issued_refresh
                + R"(","expires_in":)" + std::to_string(issued_expires_in)
                + "}");
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

    LoopbackHttpServer http_;
    std::atomic<int> generation_ { 0 };
};

// Acts as the user's browser for OAuth flows: opens the authorize URL,
// follows the redirect into the client's loopback callback listener.
inline bool oauth_fake_browser(const std::string& url)
{
    std::vector<std::string> response_headers;
    std::string body;
    long code = 0;
    imza::HttpPostOptions post_opts { };
    post_opts.max_redirs       = 0;
    post_opts.response_headers = &response_headers;
    if (imza::http_post(url, { }, "", 5, body, &code, post_opts)
            != imza::Status::OK
        || code != 302) {
        return false;
    }
    std::string location;
    for (const std::string& line : response_headers) {
        if (line.rfind("Location: ", 0) == 0) {
            location = imza::trim(std::string_view(line).substr(10));
            break;
        }
    }
    if (location.empty()) {
        return false;
    }
    imza::HttpGetOptions get_opts { };
    get_opts.max_redirs = 0;
    long get_code       = 0;
    return imza::http_get(location, { }, 5, body, &get_code, get_opts)
        == imza::Status::OK;
}

} // namespace imza::test
