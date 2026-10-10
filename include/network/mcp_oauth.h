#pragma once

#include "common/types.h"
#include "network/network.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace imza {

// OAuth 2.1 authorization for MCP HTTP servers: RFC 8414 metadata
// discovery, RFC 7591 dynamic client registration, RFC 7636 PKCE (S256),
// RFC 8252 loopback redirect, RFC 8707 resource indicators. Free
// functions over injectable transport and effects; every call blocks on
// the calling thread, and sequencing/thread choice belong to the caller.

// Injectable transport for tests; empty hooks fall back to the real
// HTTP transport (http_get/http_post with redirects disabled).
using McpOauthGet  = std::function<Status(const std::string& url,
    const std::vector<std::string>& headers, long timeout_secs,
    std::string& body, long* http_code)>;
using McpOauthPost = std::function<Status(const std::string& url,
    const std::vector<std::string>& headers, const std::string& payload,
    long timeout_secs, std::string& body, long* http_code,
    std::vector<std::string>* response_headers)>;

struct McpProtectedResourceMetadata {
    std::optional<std::string> resource; // "resource"
    std::optional<std::vector<std::string>>
        authorization_servers; // "authorization_servers"
};

struct McpAuthorizationServerMetadata {
    std::optional<std::string> issuer;  // "issuer"
    std::string authorization_endpoint; // "authorization_endpoint"
    std::string token_endpoint;         // "token_endpoint"
    std::optional<std::string> registration_endpoint; // RFC 7591
    std::optional<std::vector<std::string>> scopes_supported;
};

// A registered OAuth client; public clients carry no secret.
struct McpOauthClient {
    std::string client_id;
    std::string client_secret; // empty unless the server issued one
};

struct McpOauthTokens {
    std::string access_token;
    std::string refresh_token;   // empty when the server issues none
    std::int64_t expires_at = 0; // unix seconds, 0 unknown
};

struct McpPkcePair {
    std::string verifier;
    std::string challenge; // S256: base64url(SHA-256(verifier)), unpadded
};

McpPkcePair mcp_pkce_generate();
// Exposed for RFC 7636 test vectors; sign-in paths use mcp_pkce_generate.
std::string mcp_pkce_challenge(std::string_view verifier);

// Value of the resource_metadata parameter inside a 401 response's
// WWW-Authenticate header, among raw response header lines
// (case-insensitive); "" when absent.
std::string mcp_resource_metadata_url(
    const std::vector<std::string>& response_headers);

// {origin}/.well-known/oauth-protected-resource for an MCP server URL.
std::string mcp_protected_resource_fallback_url(std::string_view server_url);

// RFC 8414 well-known URL of an issuer: the well-known path inserts
// between the issuer path and query.
std::string mcp_as_metadata_url(std::string_view issuer);

Status mcp_fetch_protected_resource(const std::string& url,
    const McpOauthGet& get, McpProtectedResourceMetadata& out,
    std::string& detail);

// True for https endpoints and http on loopback hosts; the config
// validation shares this policy.
bool mcp_oauth_endpoint_allowed(std::string_view url);

Status mcp_fetch_authorization_server(const std::string& issuer,
    const McpOauthGet& get, McpAuthorizationServerMetadata& out,
    std::string& detail);

// RFC 7591 dynamic client registration for a public client.
Status mcp_register_client(const std::string& registration_endpoint,
    const std::string& redirect_uri, const McpOauthPost& post,
    McpOauthClient& out, std::string& detail);

// Authorization-code request URL (scope only when the server advertises
// scopes; `resource` per RFC 8707).
std::string mcp_authorize_url(const McpAuthorizationServerMetadata& as,
    const McpOauthClient& client, std::string_view redirect_uri,
    std::string_view state, std::string_view challenge,
    std::string_view resource);

Status mcp_exchange_code(const std::string& token_endpoint,
    const McpOauthClient& client, std::string_view code,
    std::string_view verifier, std::string_view redirect_uri,
    const McpOauthPost& post, McpOauthTokens& out, std::string& detail);

Status mcp_refresh_tokens(const std::string& token_endpoint,
    const McpOauthClient& client, std::string_view refresh_token,
    const McpOauthPost& post, McpOauthTokens& out, std::string& detail);

// One-shot loopback redirect listener (RFC 8252): binds 127.0.0.1 on an
// OS-chosen port and answers exactly one authorization callback. The
// OS-specific socket descriptor is held as an integer; invalid until
// start() succeeds.
class McpLoopbackListener {
public:
    McpLoopbackListener() = default;
    ~McpLoopbackListener();

    McpLoopbackListener(const McpLoopbackListener&)            = delete;
    McpLoopbackListener& operator=(const McpLoopbackListener&) = delete;

    bool start();
    bool valid() const;
    std::string redirect_uri() const; // http://127.0.0.1:<port>/callback

    struct Callback {
        std::string code;  // authorization code (empty on error)
        std::string state; // echoed state parameter
        std::string error; // OAuth error parameter when the AS denied
    };

    // Accepts callback GETs until one carries `state`, the `error`
    // parameter, the deadline, or `cancel`. Mismatched-state requests
    // are answered and discarded. Status::OK fills `out`; TIMEOUT,
    // CANCELLED, and NETWORK_ERROR set `detail`.
    Status wait_for_callback(const std::string& state, long timeout_secs,
        const std::atomic_bool& cancel, Callback& out, std::string& detail);

private:
    void close();

    std::intptr_t listen_ = -1; // POSIX fd or WinSOCKET
    int port_             = 0;
};

struct McpSignInHooks {
    McpOauthGet get;
    McpOauthPost post;
    // Opens the user's browser at the authorize URL. Defaults to failing:
    // the network layer has no process-spawning dependency. Sign-in keeps
    // waiting for the callback either way — the user may open the URL
    // (surfaced through the outcome/progress) by hand.
    std::function<bool(const std::string& url)> open_browser;
    std::function<void(std::string_view phase)> progress;
    long browser_timeout_secs = 300;
};

struct McpSignInOutcome {
    Status status = Status::OK;
    std::string detail;
    McpAuthorizationServerMetadata as;
    McpOauthClient client;
    McpOauthTokens tokens;
    // Set once the browser phase starts; kept on later failures so the
    // caller can offer the URL for manual opening.
    std::string authorize_url;
};

// Drives the full browser sign-in for one MCP server URL: unauthenticated
// probe, metadata discovery, client registration (or reuse through
// `existing` when the AS offers no registration endpoint), PKCE + state +
// loopback listener, browser phase, and the code exchange.
McpSignInOutcome mcp_oauth_sign_in(std::string_view server_url,
    const McpOauthClient* existing, const McpSignInHooks& hooks,
    const std::atomic_bool& cancel);

} // namespace imza
