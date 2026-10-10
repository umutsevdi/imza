#include <doctest/doctest.h>

#include "loopback_mcp_server.h"
#include "loopback_oauth_server.h"
#include "network/mcp_oauth.h"

#include <atomic>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

using namespace imza;
using namespace imza::test;

namespace {

// Acts as the user's browser: opens the authorize URL, follows the
// redirect to the loopback callback, and reports whether that succeeded.
bool fake_browser(const std::string& url)
{
    std::vector<std::string> response_headers;
    std::string body;
    long code = 0;
    HttpPostOptions post_opts { };
    post_opts.max_redirs       = 0;
    post_opts.response_headers = &response_headers;
    if (http_post(url, { }, "", 5, body, &code, post_opts) != Status::OK
        || code != 302) {
        return false;
    }
    std::string location;
    for (const std::string& line : response_headers) {
        if (line.rfind("Location: ", 0) == 0) {
            location = trim(std::string_view(line).substr(10));
            break;
        }
    }
    if (location.empty()) {
        return false;
    }
    HttpGetOptions get_opts { };
    get_opts.max_redirs = 0;
    return http_get(location, { }, 5, body, &code, get_opts) == Status::OK;
}

McpSignInHooks browser_hooks()
{
    McpSignInHooks hooks;
    hooks.open_browser
        = [](const std::string& url) { return fake_browser(url); };
    hooks.browser_timeout_secs = 10;
    return hooks;
}

} // namespace

TEST_CASE("pkce matches the RFC 7636 appendix B vector")
{
    CHECK(mcp_pkce_challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk")
        == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
}

TEST_CASE("generated pkce pair is self-consistent")
{
    const McpPkcePair pair = mcp_pkce_generate();
    CHECK(pair.verifier.size() == 64);
    CHECK(mcp_pkce_challenge(pair.verifier) == pair.challenge);
    const McpPkcePair other = mcp_pkce_generate();
    CHECK(other.verifier != pair.verifier);
}

TEST_CASE("resource_metadata is parsed out of WWW-Authenticate")
{
    CHECK(mcp_resource_metadata_url(
              { "WWW-Authenticate: Bearer realm=\"mcp\", "
                "resource_metadata=\"https://as.example/.well-known/xyz\"" })
        == "https://as.example/.well-known/xyz");
    // Header names are case-insensitive on the wire.
    CHECK(mcp_resource_metadata_url(
              { "www-authenticate: Bearer "
                "resource_metadata=\"https://as.example/pr\"" })
        == "https://as.example/pr");
    CHECK(mcp_resource_metadata_url(
        { "Content-Type: application/json", "WWW-Authenticate: Bearer" })
            .empty());
    CHECK(mcp_resource_metadata_url({ }).empty());
}

TEST_CASE("well-known metadata urls follow RFC 8414")
{
    CHECK(mcp_as_metadata_url("https://auth.example.com")
        == "https://auth.example.com/.well-known/oauth-authorization-server");
    CHECK(mcp_as_metadata_url("https://auth.example.com/")
        == "https://auth.example.com/.well-known/oauth-authorization-server");
    CHECK(mcp_as_metadata_url("https://auth.example.com/oauth?q=1")
        == "https://auth.example.com/.well-known/oauth-authorization-server/"
           "oauth?q=1");
    CHECK(mcp_protected_resource_fallback_url("https://mcp.example.com/mcp")
        == "https://mcp.example.com/.well-known/oauth-protected-resource");
}

TEST_CASE("authorize url carries code flow, pkce, resource, and scopes")
{
    McpAuthorizationServerMetadata as;
    as.authorization_endpoint = "https://as.example/authorize";
    as.scopes_supported       = std::vector<std::string> { "read", "write" };
    const McpOauthClient client { "client id", "" };
    const std::string url
        = mcp_authorize_url(as, client, "http://127.0.0.1:8080/callback",
            "st ate", "chal lenge", "https://mcp.example.com/mcp");
    CHECK(url.find("https://as.example/authorize?") == 0);
    CHECK(url.find("response_type=code") != std::string::npos);
    CHECK(url.find("client_id=client%20id") != std::string::npos);
    CHECK(url.find("redirect_uri=http%3A%2F%2F127.0.0.1%3A8080%2Fcallback")
        != std::string::npos);
    CHECK(url.find("state=st%20ate") != std::string::npos);
    CHECK(url.find("code_challenge=chal%20lenge") != std::string::npos);
    CHECK(url.find("code_challenge_method=S256") != std::string::npos);
    CHECK(url.find("scope=read%20write") != std::string::npos);
    CHECK(url.find("resource=https%3A%2F%2Fmcp.example.com%2Fmcp")
        != std::string::npos);
    // No advertised scopes: the scope parameter is omitted entirely.
    as.scopes_supported.reset();
    const std::string bare = mcp_authorize_url(as, client,
        "http://127.0.0.1:1/callback", "s", "c", "https://mcp.example.com/mcp");
    CHECK(bare.find("scope=") == std::string::npos);
}

TEST_CASE("exchange and refresh post the RFC 6749 form shape")
{
    std::string posted_url;
    std::string posted_body;
    const McpOauthPost post
        = [&](const std::string& url, const std::vector<std::string>&,
              const std::string& payload, long, std::string& body, long* code,
              std::vector<std::string>*) {
              posted_url  = url;
              posted_body = payload;
              *code       = 200;
              body        = R"({"access_token":"A","refresh_token":"R2",)"
                            R"("expires_in":60})";
              return Status::OK;
          };

    const McpOauthClient client { "cid", "csecret" };
    McpOauthTokens tokens;
    std::string detail;
    REQUIRE(
        mcp_exchange_code("https://as.example/token", client, "the code",
            "the verifier", "http://127.0.0.1:9/callback", post, tokens, detail)
        == Status::OK);
    CHECK(posted_url == "https://as.example/token");
    CHECK(posted_body.find("grant_type=authorization_code") == 0);
    CHECK(posted_body.find("code=the%20code") != std::string::npos);
    CHECK(posted_body.find("redirect_uri=http%3A%2F%2F127.0.0.1%3A9%2Fcallback")
        != std::string::npos);
    CHECK(posted_body.find("client_id=cid") != std::string::npos);
    CHECK(posted_body.find("client_secret=csecret") != std::string::npos);
    CHECK(
        posted_body.find("code_verifier=the%20verifier") != std::string::npos);
    CHECK(tokens.access_token == "A");
    CHECK(tokens.refresh_token == "R2");
    CHECK(tokens.expires_at > static_cast<std::int64_t>(std::time(nullptr)));

    const McpOauthPost deny
        = [&](const std::string&, const std::vector<std::string>&,
              const std::string&, long, std::string& body, long* code,
              std::vector<std::string>*) {
              *code = 400;
              body  = R"({"error":"invalid_grant"})";
              return Status::OK;
          };
    McpOauthTokens denied;
    CHECK(mcp_refresh_tokens(
              "https://as.example/token", client, "R2", deny, denied, detail)
        == Status::API_ERROR);
    CHECK(detail == "invalid_grant");

    // A refresh response without a refresh token keeps the stored one.
    const McpOauthPost rotate_free
        = [&](const std::string&, const std::vector<std::string>&,
              const std::string& payload, long, std::string& body, long* code,
              std::vector<std::string>*) {
              posted_body = payload;
              *code       = 200;
              body        = R"({"access_token":"B","expires_in":60})";
              return Status::OK;
          };
    McpOauthTokens refreshed;
    REQUIRE(mcp_refresh_tokens("https://as.example/token", client, "R2",
                rotate_free, refreshed, detail)
        == Status::OK);
    CHECK(refreshed.access_token == "B");
    CHECK(refreshed.refresh_token == "R2");
    CHECK(posted_body.find("grant_type=refresh_token") == 0);
    CHECK(posted_body.find("refresh_token=R2") != std::string::npos);
    CHECK(posted_body.find("client_id=cid") != std::string::npos);
}

TEST_CASE("loopback listener accepts the matching callback")
{
    allow_loopback_direct();
    McpLoopbackListener listener;
    REQUIRE(listener.start());
    CHECK(listener.redirect_uri().rfind("http://127.0.0.1:", 0) == 0);
    CHECK(listener.redirect_uri().find("/callback") != std::string::npos);

    std::thread opener([&listener] {
        std::string body;
        long code = 0;
        HttpGetOptions opts { };
        opts.max_redirs = 0;
        http_get(listener.redirect_uri() + "?code=abc&state=good", { }, 5, body,
            &code, opts);
    });
    McpLoopbackListener::Callback callback;
    std::string detail;
    std::atomic_bool cancel { false };
    CHECK(listener.wait_for_callback("good", 5, cancel, callback, detail)
        == Status::OK);
    CHECK(callback.code == "abc");
    CHECK(callback.state == "good");
    opener.join();
}

TEST_CASE("loopback listener skips mismatched state and keeps waiting")
{
    allow_loopback_direct();
    McpLoopbackListener listener;
    REQUIRE(listener.start());

    std::thread opener([&listener] {
        std::string body;
        long code = 0;
        HttpGetOptions opts { };
        opts.max_redirs = 0;
        // A request with someone else's state: answered, then ignored.
        http_get(listener.redirect_uri() + "?code=first&state=wrong", { }, 5,
            body, &code, opts);
        http_get(listener.redirect_uri() + "?code=second&state=good", { }, 5,
            body, &code, opts);
    });
    McpLoopbackListener::Callback callback;
    std::string detail;
    std::atomic_bool cancel { false };
    CHECK(listener.wait_for_callback("good", 5, cancel, callback, detail)
        == Status::OK);
    CHECK(callback.code == "second");
    opener.join();
}

TEST_CASE("loopback listener honours the deadline and cancellation")
{
    allow_loopback_direct();
    McpLoopbackListener listener;
    REQUIRE(listener.start());
    McpLoopbackListener::Callback callback;
    std::string detail;
    std::atomic_bool cancel { false };
    CHECK(listener.wait_for_callback("s", 1, cancel, callback, detail)
        == Status::TIMEOUT);

    cancel.store(true);
    CHECK(listener.wait_for_callback("s", 60, cancel, callback, detail)
        == Status::CANCELLED);
}

TEST_CASE("sign-in completes against a loopback authorization server")
{
    allow_loopback_direct();
    LoopbackOauthServer server;
    REQUIRE(server.start());

    McpSignInHooks hooks = browser_hooks();
    std::vector<std::string> phases;
    hooks.progress
        = [&phases](std::string_view phase) { phases.emplace_back(phase); };
    std::atomic_bool cancel { false };
    const McpSignInOutcome outcome
        = mcp_oauth_sign_in(server.mcp_url(), nullptr, hooks, cancel);
    REQUIRE(outcome.status == Status::OK);
    CHECK(outcome.tokens.access_token == "tok-1");
    CHECK(outcome.tokens.refresh_token == "refresh-ok");
    CHECK(outcome.tokens.expires_at
        > static_cast<std::int64_t>(std::time(nullptr)));
    CHECK(outcome.client.client_id == "client-test");
    CHECK(outcome.as.token_endpoint == server.base_url() + "/token");
    CHECK(server.saw_code_verifier.load());
    CHECK(server.saw_redirect_uri.load());
    CHECK(server.exchange_count.load() == 1);
    CHECK(outcome.authorize_url.find("code_challenge_method=S256")
        != std::string::npos);
    CHECK(outcome.authorize_url.find("resource=") != std::string::npos);
    REQUIRE(!phases.empty());
    CHECK(phases.back() == "finishing sign-in");
}

TEST_CASE("sign-in discovers metadata without the WWW-Authenticate header")
{
    allow_loopback_direct();
    LoopbackOauthServer server;
    server.no_metadata_header = true;
    REQUIRE(server.start());
    McpSignInHooks hooks = browser_hooks();
    std::atomic_bool cancel { false };
    CHECK(mcp_oauth_sign_in(server.mcp_url(), nullptr, hooks, cancel).status
        == Status::OK);
    CHECK(server.unauthorized_mcp.load() > 0);
}

TEST_CASE("sign-in fails cleanly when the server needs no authorization")
{
    allow_loopback_direct();
    LoopbackOauthServer server;
    server.require_bearer = false;
    REQUIRE(server.start());
    McpSignInHooks hooks = browser_hooks();
    std::atomic_bool cancel { false };
    const McpSignInOutcome outcome
        = mcp_oauth_sign_in(server.mcp_url(), nullptr, hooks, cancel);
    CHECK(outcome.status == Status::CONFIG_ERROR);
    CHECK(outcome.detail.find("does not require sign-in") != std::string::npos);
}

TEST_CASE("sign-in needs registration or a stored client")
{
    allow_loopback_direct();
    LoopbackOauthServer server;
    server.no_registration = true;
    REQUIRE(server.start());
    McpSignInHooks hooks = browser_hooks();
    std::atomic_bool cancel { false };
    const McpSignInOutcome outcome
        = mcp_oauth_sign_in(server.mcp_url(), nullptr, hooks, cancel);
    CHECK(outcome.status == Status::CONFIG_ERROR);
    CHECK(outcome.detail.find("pre-registered client") != std::string::npos);
    CHECK(!server.saw_register.load());

    // With a stored client the flow completes without any registration.
    LoopbackOauthServer reuse;
    reuse.no_registration = true;
    REQUIRE(reuse.start());
    const McpOauthClient existing { "client-test", "" };
    const McpSignInOutcome reused
        = mcp_oauth_sign_in(reuse.mcp_url(), &existing, hooks, cancel);
    CHECK(reused.status == Status::OK);
    CHECK(reused.client.client_id == "client-test");
    CHECK(!reuse.saw_register.load());
}

TEST_CASE("sign-in surfaces registration, consent, and grant failures")
{
    allow_loopback_direct();
    std::atomic_bool cancel { false };
    McpSignInHooks hooks = browser_hooks();

    LoopbackOauthServer registration_down;
    registration_down.registration_fails = true;
    REQUIRE(registration_down.start());
    const McpSignInOutcome failed_registration = mcp_oauth_sign_in(
        registration_down.mcp_url(), nullptr, hooks, cancel);
    CHECK(failed_registration.status == Status::API_ERROR);
    CHECK(failed_registration.detail.find("registering") != std::string::npos);

    LoopbackOauthServer denied;
    denied.deny_authorization = true;
    REQUIRE(denied.start());
    const McpSignInOutcome refused
        = mcp_oauth_sign_in(denied.mcp_url(), nullptr, hooks, cancel);
    CHECK(refused.status == Status::API_ERROR);
    CHECK(refused.detail.find("access_denied") != std::string::npos);
    // The authorize URL stays available for manual opening.
    CHECK(!refused.authorize_url.empty());

    LoopbackOauthServer grant;
    grant.invalid_grant = true;
    REQUIRE(grant.start());
    const McpSignInOutcome bad_grant
        = mcp_oauth_sign_in(grant.mcp_url(), nullptr, hooks, cancel);
    CHECK(bad_grant.status == Status::API_ERROR);
    CHECK(bad_grant.detail == "invalid_grant");
    CHECK(!bad_grant.authorize_url.empty());
}

TEST_CASE("sign-in is cancellable while waiting for the browser")
{
    allow_loopback_direct();
    LoopbackOauthServer server;
    REQUIRE(server.start());
    McpSignInHooks hooks = browser_hooks();
    // No browser opens; cancel as soon as the wait starts.
    hooks.open_browser = [](const std::string&) { return false; };
    std::atomic_bool cancel { false };
    hooks.progress = [&cancel](std::string_view phase) {
        if (phase.find("waiting for the browser") == 0) {
            cancel.store(true);
        }
    };
    const McpSignInOutcome outcome
        = mcp_oauth_sign_in(server.mcp_url(), nullptr, hooks, cancel);
    CHECK(outcome.status == Status::CANCELLED);
    CHECK(outcome.detail.find("cancelled") != std::string::npos);
    CHECK(!outcome.authorize_url.empty());
}
