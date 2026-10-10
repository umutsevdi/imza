#define NOMINMAX

#include "network/mcp_oauth.h"

#include "common/util.h"
#include "network/json.h"
#include "network/json_io.h"
#include "network/mcp.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <random>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace imza {

// Glaze-reflected: must have external linkage (Clang/MSVC requirement).
struct McpRegistrationRequest {
    std::vector<std::string> redirect_uris;
    std::vector<std::string> grant_types;
    std::string client_name;
    std::string token_endpoint_auth_method;
};

struct McpRegistrationResponse {
    std::optional<std::string> client_id;
    std::optional<std::string> client_secret;
    std::optional<std::string> error;
    std::optional<std::string> error_description;
};

struct McpTokenResponse {
    std::optional<std::string> access_token;
    std::optional<std::string> refresh_token;
    std::optional<std::int64_t> expires_in;
    std::optional<std::string> error;
    std::optional<std::string> error_description;
};

namespace {

    constexpr long METADATA_TIMEOUT_SECS             = 15;
    constexpr long TOKEN_TIMEOUT_SECS                = 30;
    constexpr std::size_t MAX_CALLBACK_REQUEST_BYTES = 8192;

#ifdef _WIN32
    void winsock_once()
    {
        static std::once_flag once;
        std::call_once(once, [] {
            WSADATA data;
            WSAStartup(MAKEWORD(2, 2), &data);
        });
    }
    using socket_t                          = SOCKET;
    constexpr socket_t INVALID_SOCKET_VALUE = INVALID_SOCKET;
    void close_socket(socket_t fd) { ::closesocket(fd); }
    // >0 readable, 0 timeout, -1 poll failure.
    int wait_readable(socket_t fd, int timeout_ms)
    {
        WSAPOLLFD poll_fd { fd, POLLRDNORM, 0 };
        return ::WSAPoll(&poll_fd, 1, timeout_ms);
    }
#else
    void winsock_once() { }
    using socket_t                          = int;
    constexpr socket_t INVALID_SOCKET_VALUE = -1;
    void close_socket(socket_t fd) { ::close(fd); }
    // >0 readable, 0 timeout, -1 poll failure (EINTR included).
    int wait_readable(socket_t fd, int timeout_ms)
    {
        pollfd poll_fd { fd, POLLIN, 0 };
        return ::poll(&poll_fd, 1, timeout_ms);
    }
#endif

    bool send_all(socket_t fd, std::string_view wire)
    {
        std::size_t sent = 0;
        while (sent < wire.size()) {
#ifdef _WIN32
            const int now     = ::send(fd, wire.data() + sent,
                static_cast<int>(wire.size() - sent), 0);
            const bool failed = now <= 0;
#else
            const ssize_t now
                = ::send(fd, wire.data() + sent, wire.size() - sent, 0);
            const bool failed = now <= 0;
#endif
            if (failed) {
                return false;
            }
            sent += static_cast<std::size_t>(now);
        }
        return true;
    }

    void set_recv_timeout(socket_t fd, int secs)
    {
#ifdef _WIN32
        const DWORD ms = static_cast<DWORD>(secs) * 1000;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
            reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
        const timeval timeout { secs, 0 };
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
    }

    // RFC 7636 verifier alphabet; the challenge is base64url of the SHA-256
    // digest without padding.
    constexpr std::string_view PKCE_CHARS
        = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";

    std::string random_string(std::size_t length)
    {
        thread_local std::mt19937_64 engine { std::random_device { }() };
        std::uniform_int_distribution<std::size_t> pick(
            0, PKCE_CHARS.size() - 1);
        std::string out;
        out.reserve(length);
        for (std::size_t i = 0; i < length; ++i) {
            out.push_back(PKCE_CHARS[pick(engine)]);
        }
        return out;
    }

    // SHA-256 (FIPS 180-4); only PKCE challenges are hashed, so a local
    // implementation avoids pulling in a crypto dependency.
    std::array<std::uint8_t, 32> sha256(std::string_view input)
    {
        constexpr std::uint32_t K[] = {
            0x428a2f98,
            0x71374491,
            0xb5c0fbcf,
            0xe9b5dba5,
            0x3956c25b,
            0x59f111f1,
            0x923f82a4,
            0xab1c5ed5,
            0xd807aa98,
            0x12835b01,
            0x243185be,
            0x550c7dc3,
            0x72be5d74,
            0x80deb1fe,
            0x9bdc06a7,
            0xc19bf174,
            0xe49b69c1,
            0xefbe4786,
            0x0fc19dc6,
            0x240ca1cc,
            0x2de92c6f,
            0x4a7484aa,
            0x5cb0a9dc,
            0x76f988da,
            0x983e5152,
            0xa831c66d,
            0xb00327c8,
            0xbf597fc7,
            0xc6e00bf3,
            0xd5a79147,
            0x06ca6351,
            0x14292967,
            0x27b70a85,
            0x2e1b2138,
            0x4d2c6dfc,
            0x53380d13,
            0x650a7354,
            0x766a0abb,
            0x81c2c92e,
            0x92722c85,
            0xa2bfe8a1,
            0xa81a664b,
            0xc24b8b70,
            0xc76c51a3,
            0xd192e819,
            0xd6990624,
            0xf40e3585,
            0x106aa070,
            0x19a4c116,
            0x1e376c08,
            0x2748774c,
            0x34b0bcb5,
            0x391c0cb3,
            0x4ed8aa4a,
            0x5b9cca4f,
            0x682e6ff3,
            0x748f82ee,
            0x78a5636f,
            0x84c87814,
            0x8cc70208,
            0x90befffa,
            0xa4506ceb,
            0xbef9a3f7,
            0xc67178f2,
        };
        std::uint32_t state[] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372,
            0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };

        const auto rotate = [](std::uint32_t value, unsigned count) {
            return (value >> count) | (value << (32 - count));
        };

        std::string message(input);
        const std::uint64_t bit_length = message.size() * 8ULL;
        message.push_back(static_cast<char>(0x80));
        while (message.size() % 64 != 56) {
            message.push_back('\0');
        }
        for (int shift = 56; shift >= 0; shift -= 8) {
            message.push_back(static_cast<char>((bit_length >> shift) & 0xff));
        }

        for (std::size_t offset = 0; offset < message.size(); offset += 64) {
            std::uint32_t w[64];
            for (std::size_t i = 0; i < 16; ++i) {
                w[i]
                    = (static_cast<std::uint8_t>(message[offset + i * 4]) << 24)
                    | (static_cast<std::uint8_t>(message[offset + i * 4 + 1])
                        << 16)
                    | (static_cast<std::uint8_t>(message[offset + i * 4 + 2])
                        << 8)
                    | static_cast<std::uint8_t>(message[offset + i * 4 + 3]);
            }
            for (std::size_t i = 16; i < 64; ++i) {
                const std::uint32_t s0 = rotate(w[i - 15], 7)
                    ^ rotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
                const std::uint32_t s1 = rotate(w[i - 2], 17)
                    ^ rotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
                w[i] = w[i - 16] + s0 + w[i - 7] + s1;
            }
            std::uint32_t a = state[0], b = state[1], c = state[2],
                          d = state[3], e = state[4], f = state[5],
                          g = state[6], h = state[7];
            for (std::size_t i = 0; i < 64; ++i) {
                const std::uint32_t sum1
                    = rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25);
                const std::uint32_t choice = (e & f) ^ (~e & g);
                const std::uint32_t temp1  = h + sum1 + choice + K[i] + w[i];
                const std::uint32_t sum0
                    = rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22);
                const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
                const std::uint32_t temp2    = sum0 + majority;
                h                            = g;
                g                            = f;
                f                            = e;
                e                            = d + temp1;
                d                            = c;
                c                            = b;
                b                            = a;
                a                            = temp1 + temp2;
            }
            state[0] += a;
            state[1] += b;
            state[2] += c;
            state[3] += d;
            state[4] += e;
            state[5] += f;
            state[6] += g;
            state[7] += h;
        }

        std::array<std::uint8_t, 32> digest { };
        for (std::size_t i = 0; i < 8; ++i) {
            for (int byte = 0; byte < 4; ++byte) {
                digest[i * 4 + byte]
                    = static_cast<std::uint8_t>(state[i] >> (24 - byte * 8));
            }
        }
        return digest;
    }

    std::string_view url_scheme(std::string_view url)
    {
        const auto separator = url.find("://");
        return separator == std::string_view::npos ? std::string_view { }
                                                   : url.substr(0, separator);
    }

    std::string url_origin(std::string_view url)
    {
        const auto separator = url.find("://");
        if (separator == std::string_view::npos) {
            return { };
        }
        const auto path_start = url.find('/', separator + 3);
        return path_start == std::string_view::npos
            ? std::string(url)
            : std::string(url.substr(0, path_start));
    }

    bool loopback_host(std::string_view url)
    {
        const std::string origin = url_origin(url);
        if (origin.empty()) {
            return false;
        }
        const auto separator  = origin.find("://");
        std::string_view host = std::string_view(origin).substr(separator + 3);
        if (host.starts_with("127.0") || host == "localhost"
            || host == "[::1]") {
            return true;
        }
        return false;
    }

} // namespace

// OAuth endpoints must be https; loopback is the exception local
// authorization servers (and the tests) rely on. Shared with the config
// validation so the policy lives once.
bool mcp_oauth_endpoint_allowed(std::string_view url)
{
    return url_scheme(url) == "https" || loopback_host(url);
}

namespace {
    Status default_get(const std::string& url,
        const std::vector<std::string>& headers, long timeout_secs,
        std::string& body, long* http_code)
    {
        HttpGetOptions opts { };
        opts.max_redirs = 0;
        return http_get(url, headers, timeout_secs, body, http_code, opts);
    }

    Status default_post(const std::string& url,
        const std::vector<std::string>& headers, const std::string& payload,
        long timeout_secs, std::string& body, long* http_code,
        std::vector<std::string>* response_headers)
    {
        HttpPostOptions opts { };
        opts.max_redirs       = 0;
        opts.response_headers = response_headers;
        return http_post(
            url, headers, payload, timeout_secs, body, http_code, opts);
    }

    McpOauthGet resolve_get(const McpOauthGet& get)
    {
        return get ? get : McpOauthGet { default_get };
    }

    McpOauthPost resolve_post(const McpOauthPost& post)
    {
        return post ? post : McpOauthPost { default_post };
    }

    // Parses a token-endpoint body; a refresh response may omit the
    // refresh token (the stored one stays valid), hence `old_refresh`.
    Status parse_token_response(const std::string& body,
        std::string_view old_refresh, McpOauthTokens& out, std::string& detail)
    {
        McpTokenResponse root;
        if (const glz::error_ctx error = json_parse_checked(body, root)) {
            detail = json_parse_error(body, error);
            return Status::JSON_ERROR;
        }
        if (root.error) {
            detail = root.error_description ? *root.error_description
                                            : *root.error;
            return Status::API_ERROR;
        }
        if (!root.access_token || root.access_token->empty()) {
            detail = "token response carried no access token";
            return Status::JSON_ERROR;
        }
        out.access_token = *root.access_token;
        out.refresh_token
            = root.refresh_token.value_or(std::string(old_refresh));
        if (root.expires_in) {
            out.expires_at = static_cast<std::int64_t>(std::time(nullptr))
                + *root.expires_in;
        }
        return Status::OK;
    }

    // OAuth failures answer 4xx with a JSON error body; prefer its
    // error/error_description over the bare HTTP status text.
    std::string error_detail(
        const std::string& body, const std::string& fallback)
    {
        McpTokenResponse root;
        if (!json_parse_checked(body, root) && root.error) {
            return root.error_description ? *root.error_description
                                          : *root.error;
        }
        return fallback;
    }

    void append_form_pair(
        std::string& form, const char* key, std::string_view value)
    {
        if (value.empty()) {
            return;
        }
        if (!form.empty()) {
            form += '&';
        }
        form += key;
        form += '=';
        form += percent_encode(value);
    }

} // namespace

std::string mcp_pkce_challenge(std::string_view verifier)
{
    const std::array<std::uint8_t, 32> digest = sha256(verifier);
    return base64url_encode(std::string_view(
        reinterpret_cast<const char*>(digest.data()), digest.size()));
}

McpPkcePair mcp_pkce_generate()
{
    McpPkcePair pair;
    pair.verifier  = random_string(64);
    pair.challenge = mcp_pkce_challenge(pair.verifier);
    return pair;
}

std::string mcp_resource_metadata_url(
    const std::vector<std::string>& response_headers)
{
    for (const std::string& line : response_headers) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        if (to_lower(std::string_view(line).substr(0, colon))
            != "www-authenticate") {
            continue;
        }
        const std::string value(trim(std::string_view(line).substr(colon + 1)));
        const std::string lowered = to_lower(value);
        const auto key            = lowered.find("resource_metadata");
        if (key == std::string::npos) {
            continue;
        }
        const auto equals = lowered.find('=', key);
        if (equals == std::string::npos) {
            continue;
        }
        std::size_t at = equals + 1;
        while (at < value.size() && value[at] == ' ') {
            ++at;
        }
        std::string url;
        if (at < value.size() && value[at] == '"') {
            const auto closing = value.find('"', at + 1);
            if (closing == std::string::npos) {
                continue;
            }
            url = value.substr(at + 1, closing - at - 1);
        } else {
            const auto end = std::min(value.find(',', at), value.find(' ', at));
            url            = value.substr(
                at, (end == std::string::npos ? value.size() : end) - at);
        }
        const std::string scheme(url_scheme(url));
        if (scheme == "http" || scheme == "https") {
            return url;
        }
    }
    return { };
}

std::string mcp_protected_resource_fallback_url(std::string_view server_url)
{
    const std::string origin = url_origin(server_url);
    return origin.empty() ? std::string { }
                          : origin + "/.well-known/oauth-protected-resource";
}

std::string mcp_as_metadata_url(std::string_view issuer)
{
    const std::string origin = url_origin(issuer);
    if (origin.empty()) {
        return { };
    }
    std::string_view rest = issuer.substr(origin.size());
    std::string_view query;
    if (const auto at = rest.find('?'); at != std::string_view::npos) {
        query = rest.substr(at);
        rest  = rest.substr(0, at);
    }
    // RFC 8414: the well-known path inserts between host and path; an
    // issuer whose whole path is "/" carries no path.
    if (rest == "/") {
        rest = { };
    }
    return origin + "/.well-known/oauth-authorization-server"
        + std::string(rest) + std::string(query);
}

Status mcp_fetch_protected_resource(const std::string& url,
    const McpOauthGet& get, McpProtectedResourceMetadata& out,
    std::string& detail)
{
    std::string body;
    long code           = 0;
    const Status status = resolve_get(get)(url, { "Accept: application/json" },
        METADATA_TIMEOUT_SECS, body, &code);
    if (status != Status::OK) {
        detail = "protected-resource metadata request failed";
        return Status::NETWORK_ERROR;
    }
    if (!http_ok(code)) {
        detail = "HTTP " + std::to_string(code)
            + " fetching protected-resource metadata";
        return Status::API_ERROR;
    }
    if (const glz::error_ctx error = json_parse_checked(body, out)) {
        detail = json_parse_error(body, error);
        return Status::JSON_ERROR;
    }
    return Status::OK;
}

Status mcp_fetch_authorization_server(const std::string& issuer,
    const McpOauthGet& get, McpAuthorizationServerMetadata& out,
    std::string& detail)
{
    const std::string url = mcp_as_metadata_url(issuer);
    if (url.empty()) {
        detail = "authorization server issuer is not a valid URL";
        return Status::CONFIG_ERROR;
    }
    std::string body;
    long code           = 0;
    const Status status = resolve_get(get)(url, { "Accept: application/json" },
        METADATA_TIMEOUT_SECS, body, &code);
    if (status != Status::OK) {
        detail = "authorization-server metadata request failed";
        return Status::NETWORK_ERROR;
    }
    if (!http_ok(code)) {
        detail = "HTTP " + std::to_string(code)
            + " fetching authorization-server metadata";
        return Status::API_ERROR;
    }
    if (const glz::error_ctx error = json_parse_checked(body, out)) {
        detail = json_parse_error(body, error);
        return Status::JSON_ERROR;
    }
    if (out.authorization_endpoint.empty() || out.token_endpoint.empty()) {
        detail = "authorization-server metadata is missing an endpoint";
        return Status::JSON_ERROR;
    }
    if (!mcp_oauth_endpoint_allowed(out.authorization_endpoint)
        || !mcp_oauth_endpoint_allowed(out.token_endpoint)) {
        detail = "authorization-server endpoints must be https";
        return Status::CONFIG_ERROR;
    }
    return Status::OK;
}

Status mcp_register_client(const std::string& registration_endpoint,
    const std::string& redirect_uri, const McpOauthPost& post,
    McpOauthClient& out, std::string& detail)
{
    McpRegistrationRequest request;
    request.redirect_uris.push_back(redirect_uri);
    request.grant_types = { "authorization_code", "refresh_token" };
    request.client_name = "Imza";
    request.token_endpoint_auth_method = "none";

    std::string body;
    long code           = 0;
    const Status status = resolve_post(post)(registration_endpoint,
        { "Content-Type: application/json", "Accept: application/json" },
        json_dump(request), METADATA_TIMEOUT_SECS, body, &code, nullptr);
    if (status != Status::OK) {
        detail = "client registration request failed";
        return Status::NETWORK_ERROR;
    }
    if (!http_ok(code)) {
        detail = error_detail(
            body, "HTTP " + std::to_string(code) + " registering the client");
        return Status::API_ERROR;
    }
    McpRegistrationResponse root;
    if (const glz::error_ctx error = json_parse_checked(body, root)) {
        detail = json_parse_error(body, error);
        return Status::JSON_ERROR;
    }
    if (root.error) {
        detail = root.error_description ? *root.error_description : *root.error;
        return Status::API_ERROR;
    }
    if (!root.client_id || root.client_id->empty()) {
        detail = "registration response carried no client id";
        return Status::JSON_ERROR;
    }
    out.client_id     = *root.client_id;
    out.client_secret = root.client_secret.value_or(std::string { });
    return Status::OK;
}

std::string mcp_authorize_url(const McpAuthorizationServerMetadata& as,
    const McpOauthClient& client, std::string_view redirect_uri,
    std::string_view state, std::string_view challenge,
    std::string_view resource)
{
    std::string url = as.authorization_endpoint;
    url += url.find('?') == std::string::npos ? '?' : '&';
    url += "response_type=code";
    url += "&client_id=" + percent_encode(client.client_id);
    url += "&redirect_uri=" + percent_encode(redirect_uri);
    url += "&state=" + percent_encode(state);
    url += "&code_challenge=" + percent_encode(challenge);
    url += "&code_challenge_method=S256";
    if (as.scopes_supported && !as.scopes_supported->empty()) {
        std::string scopes;
        for (const std::string& scope : *as.scopes_supported) {
            if (!scopes.empty()) {
                scopes += ' ';
            }
            scopes += scope;
        }
        url += "&scope=" + percent_encode(scopes);
    }
    if (!resource.empty()) {
        url += "&resource=" + percent_encode(resource);
    }
    return url;
}

Status mcp_exchange_code(const std::string& token_endpoint,
    const McpOauthClient& client, std::string_view code,
    std::string_view verifier, std::string_view redirect_uri,
    const McpOauthPost& post, McpOauthTokens& out, std::string& detail)
{
    std::string form = "grant_type=authorization_code";
    append_form_pair(form, "code", code);
    append_form_pair(form, "redirect_uri", redirect_uri);
    append_form_pair(form, "client_id", client.client_id);
    append_form_pair(form, "client_secret", client.client_secret);
    append_form_pair(form, "code_verifier", verifier);

    std::string body;
    long code_http      = 0;
    const Status status = resolve_post(post)(token_endpoint,
        { "Content-Type: application/x-www-form-urlencoded",
            "Accept: application/json" },
        form, TOKEN_TIMEOUT_SECS, body, &code_http, nullptr);
    if (status != Status::OK) {
        detail = "token exchange request failed";
        return Status::NETWORK_ERROR;
    }
    if (!http_ok(code_http)) {
        detail = error_detail(body,
            "HTTP " + std::to_string(code_http)
                + " exchanging the authorization code");
        return Status::API_ERROR;
    }
    const Status parsed = parse_token_response(body, "", out, detail);
    return parsed;
}

Status mcp_refresh_tokens(const std::string& token_endpoint,
    const McpOauthClient& client, std::string_view refresh_token,
    const McpOauthPost& post, McpOauthTokens& out, std::string& detail)
{
    std::string form = "grant_type=refresh_token";
    append_form_pair(form, "refresh_token", refresh_token);
    append_form_pair(form, "client_id", client.client_id);
    append_form_pair(form, "client_secret", client.client_secret);

    std::string body;
    long code           = 0;
    const Status status = resolve_post(post)(token_endpoint,
        { "Content-Type: application/x-www-form-urlencoded",
            "Accept: application/json" },
        form, TOKEN_TIMEOUT_SECS, body, &code, nullptr);
    if (status != Status::OK) {
        detail = "token refresh request failed";
        return Status::NETWORK_ERROR;
    }
    if (!http_ok(code)) {
        detail = error_detail(
            body, "HTTP " + std::to_string(code) + " refreshing the token");
        return Status::API_ERROR;
    }
    const Status parsed
        = parse_token_response(body, refresh_token, out, detail);
    return parsed;
}

McpLoopbackListener::~McpLoopbackListener() { close(); }

bool McpLoopbackListener::start()
{
    winsock_once();
    socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET_VALUE) {
        return false;
    }
    int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
        reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in address { };
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port        = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0
        || ::listen(fd, 4) < 0) {
        close_socket(fd);
        return false;
    }
    sockaddr_in bound { };
    socklen_t length = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &length) < 0) {
        close_socket(fd);
        return false;
    }
    port_   = ntohs(bound.sin_port);
    listen_ = static_cast<std::intptr_t>(fd);
    return true;
}

bool McpLoopbackListener::valid() const { return listen_ != -1; }

std::string McpLoopbackListener::redirect_uri() const
{
    return "http://127.0.0.1:" + std::to_string(port_) + "/callback";
}

void McpLoopbackListener::close()
{
    if (listen_ != -1) {
        close_socket(static_cast<socket_t>(listen_));
        listen_ = -1;
    }
}

namespace {

    void respond_callback_page(socket_t fd, const std::string& body)
    {
        std::string wire
            = "HTTP/1.1 200 OK\r\nConnection: close\r\n"
              "Content-Type: text/html; charset=utf-8\r\nContent-Length: "
            + std::to_string(body.size()) + "\r\n\r\n" + body;
        send_all(fd, wire);
    }

    // Reads until the end of the request headers; callback GETs carry no
    // body. False on transport failure.
    bool read_callback_request(
        socket_t fd, std::string& method, std::string& target)
    {
        std::string buffer;
        char chunk[1024];
        while (buffer.find("\r\n\r\n") == std::string::npos) {
#ifdef _WIN32
            const int received = ::recv(fd, chunk, sizeof(chunk), 0);
            const bool failed  = received <= 0;
#else
            const ssize_t received = ::recv(fd, chunk, sizeof(chunk), 0);
            const bool failed      = received <= 0;
#endif
            if (failed) {
                return false;
            }
            buffer.append(chunk, static_cast<std::size_t>(received));
            if (buffer.size() > MAX_CALLBACK_REQUEST_BYTES) {
                return false;
            }
        }
        const auto line_end = buffer.find("\r\n");
        const auto first    = buffer.find(' ');
        if (line_end == std::string::npos || first == std::string::npos
            || first > line_end) {
            return false;
        }
        const auto second = buffer.find(' ', first + 1);
        if (second == std::string::npos || second > line_end) {
            return false;
        }
        method = buffer.substr(0, first);
        target = buffer.substr(first + 1, second - first - 1);
        return true;
    }

    // Query parameter lookup ("code", "state", "error"); "" when absent.
    std::string query_parameter(std::string_view target, std::string_view key)
    {
        const auto query = target.find('?');
        if (query == std::string_view::npos) {
            return { };
        }
        std::string_view rest = target.substr(query + 1);
        while (!rest.empty()) {
            const auto ampersand        = rest.find('&');
            const std::string_view pair = rest.substr(0,
                ampersand == std::string_view::npos ? rest.size() : ampersand);
            const auto equals           = pair.find('=');
            if (equals != std::string_view::npos
                && pair.substr(0, equals) == key) {
                return percent_decode(pair.substr(equals + 1));
            }
            rest = ampersand == std::string_view::npos
                ? std::string_view { }
                : rest.substr(ampersand + 1);
        }
        return { };
    }

} // namespace

Status McpLoopbackListener::wait_for_callback(const std::string& state,
    long timeout_secs, const std::atomic_bool& cancel, Callback& out,
    std::string& detail)
{
    const auto deadline
        = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_secs);
    const socket_t listen_fd = static_cast<socket_t>(listen_);
    for (;;) {
        if (cancel.load()) {
            detail = "cancelled";
            return Status::CANCELLED;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            detail = "timed out waiting for the browser";
            return Status::TIMEOUT;
        }
        const int readable = wait_readable(listen_fd, 250);
        if (readable == 0) {
            continue; // poll timeout; re-check the deadline and cancel
        }
        if (readable < 0 && errno != EINTR) {
            detail = "callback listener failed";
            return Status::NETWORK_ERROR;
        }
        if (readable < 0) {
            continue;
        }
        const socket_t fd = ::accept(listen_fd, nullptr, nullptr);
        if (fd == INVALID_SOCKET_VALUE) {
            continue;
        }
        set_recv_timeout(fd, 5);
        std::string method;
        std::string target;
        const bool read_ok = read_callback_request(fd, method, target);
        if (read_ok && method != "GET") {
            respond_callback_page(
                fd, "<html><body>Unexpected request.</body></html>");
        } else if (read_ok && target.rfind("/callback", 0) != 0) {
            respond_callback_page(
                fd, "<html><body>Not the sign-in callback.</body></html>");
        } else if (read_ok) {
            const std::string error = query_parameter(target, "error");
            if (!error.empty()) {
                out.error = error;
                out.state = query_parameter(target, "state");
                respond_callback_page(fd,
                    "<html><body>The service refused the sign-in. You "
                    "can close this tab.</body></html>");
                close_socket(fd);
                return Status::OK;
            }
            const std::string code      = query_parameter(target, "code");
            const std::string got_state = query_parameter(target, "state");
            if (!code.empty() && got_state == state) {
                out.code  = code;
                out.state = got_state;
                respond_callback_page(fd,
                    "<html><body>Signed in to Imza. You can close this "
                    "tab.</body></html>");
                close_socket(fd);
                return Status::OK;
            }
            // Wrong state: answer, then keep waiting for the real one.
            respond_callback_page(fd,
                "<html><body>Still waiting for the Imza sign-in to "
                "finish.</body></html>");
        }
        close_socket(fd);
    }
}

McpSignInOutcome mcp_oauth_sign_in(std::string_view server_url,
    const McpOauthClient* existing, const McpSignInHooks& hooks,
    const std::atomic_bool& cancel)
{
    McpSignInOutcome outcome;
    const McpOauthGet get   = resolve_get(hooks.get);
    const McpOauthPost post = resolve_post(hooks.post);
    const auto phase        = [&](std::string_view text) {
        if (hooks.progress) {
            hooks.progress(text);
        }
    };
    const auto fail = [&](Status status, std::string detail_text) {
        outcome.status = status;
        outcome.detail = std::move(detail_text);
        return outcome;
    };

    phase("checking the server");
    JsonValue client_info;
    client_info["name"] = "imza";
    JsonValue params;
    params["protocolVersion"] = MCP_PROTOCOL_VERSION;
    params["capabilities"]    = JsonValue(JsonValue::object_t { });
    params["clientInfo"]      = std::move(client_info);
    const std::string probe   = mcp_rpc_request(1, "initialize", params);

    std::string body;
    long code = 0;
    std::vector<std::string> response_headers;
    const Status probe_status = post(std::string(server_url),
        { "Content-Type: application/json",
            "Accept: application/json, "
            "text/event-stream" },
        probe, METADATA_TIMEOUT_SECS, body, &code, &response_headers);
    if (probe_status != Status::OK) {
        return fail(Status::NETWORK_ERROR, "could not reach the server");
    }
    if (http_ok(code)) {
        return fail(
            Status::CONFIG_ERROR, "the server does not require sign-in");
    }
    if (code != 401) {
        return fail(Status::API_ERROR,
            "HTTP " + std::to_string(code) + " from the server");
    }

    std::string metadata_url = mcp_resource_metadata_url(response_headers);
    if (metadata_url.empty()) {
        metadata_url = mcp_protected_resource_fallback_url(server_url);
    }
    McpProtectedResourceMetadata protected_resource;
    const Status fetched = mcp_fetch_protected_resource(
        metadata_url, get, protected_resource, outcome.detail);
    if (fetched != Status::OK) {
        return fail(fetched, outcome.detail);
    }
    if (!protected_resource.authorization_servers
        || protected_resource.authorization_servers->empty()) {
        return fail(
            Status::CONFIG_ERROR, "the server lists no authorization server");
    }

    phase("contacting the authorization server");
    const std::string issuer
        = protected_resource.authorization_servers->front();
    const Status as_fetched = mcp_fetch_authorization_server(
        issuer, get, outcome.as, outcome.detail);
    if (as_fetched != Status::OK) {
        return fail(as_fetched, outcome.detail);
    }

    const McpPkcePair pkce  = mcp_pkce_generate();
    const std::string state = random_string(24);
    McpLoopbackListener listener;
    if (!listener.start()) {
        return fail(
            Status::NETWORK_ERROR, "could not open a local callback listener");
    }

    if (outcome.as.registration_endpoint
        && !outcome.as.registration_endpoint->empty()) {
        phase("registering the client");
        const Status registered
            = mcp_register_client(*outcome.as.registration_endpoint,
                listener.redirect_uri(), post, outcome.client, outcome.detail);
        if (registered != Status::OK) {
            return fail(registered, outcome.detail);
        }
    } else if (existing && !existing->client_id.empty()) {
        outcome.client = *existing;
    } else {
        return fail(Status::CONFIG_ERROR,
            "the service requires a pre-registered client");
    }

    outcome.authorize_url = mcp_authorize_url(outcome.as, outcome.client,
        listener.redirect_uri(), state, pkce.challenge, server_url);

    phase("opening the browser");
    const bool opened
        = hooks.open_browser && hooks.open_browser(outcome.authorize_url);
    phase(opened ? "waiting for the browser"
                 : "waiting for the browser - open the sign-in link by hand");

    McpLoopbackListener::Callback callback;
    const Status waited = listener.wait_for_callback(
        state, hooks.browser_timeout_secs, cancel, callback, outcome.detail);
    if (waited != Status::OK) {
        return fail(waited, outcome.detail);
    }
    if (!callback.error.empty()) {
        return fail(Status::API_ERROR,
            "the service refused the sign-in: " + callback.error);
    }

    phase("finishing sign-in");
    const Status exchanged = mcp_exchange_code(outcome.as.token_endpoint,
        outcome.client, callback.code, pkce.verifier, listener.redirect_uri(),
        post, outcome.tokens, outcome.detail);
    if (exchanged != Status::OK) {
        return fail(exchanged, outcome.detail);
    }
    return outcome;
}

} // namespace imza
