#include "common/sha256.h"
#include "common/util.h"
#include "network/json.h"
#include "network/json_io.h"
#include "network/network.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <string>
namespace imza {
namespace {
    std::array<std::uint8_t, 32> hmac_sha256(
        std::string_view key, std::string_view message)
    {
        std::array<std::uint8_t, 64> block { };
        if (key.size() > block.size()) {
            const auto hashed = sha256(key);
            std::copy(hashed.begin(), hashed.end(), block.begin());
        } else {
            std::copy(key.begin(), key.end(), block.begin());
        }
        std::array<std::uint8_t, 64> inner { };
        std::string padded;
        padded.reserve(64 + message.size());
        for (std::size_t i = 0; i < block.size(); ++i) {
            inner[i] = block[i] ^ 0x36;
            block[i] ^= 0x5c;
            padded.push_back(static_cast<char>(inner[i]));
        }
        padded.append(message);
        const auto inner_digest = sha256(padded);
        std::string outer;
        outer.reserve(64 + inner_digest.size());
        outer.append(reinterpret_cast<const char*>(block.data()), block.size());
        outer.append(reinterpret_cast<const char*>(inner_digest.data()),
            inner_digest.size());
        return sha256(outer);
    }

    std::string hex(const std::array<std::uint8_t, 32>& digest)
    {
        static constexpr char DIGITS[] = "0123456789abcdef";
        std::string out;
        out.reserve(digest.size() * 2);
        for (const std::uint8_t byte : digest) {
            out.push_back(DIGITS[byte >> 4]);
            out.push_back(DIGITS[byte & 0xf]);
        }
        return out;
    }

    // AWS's URI-encoding rules: every character except [A-Za-z0-9-._~]
    // becomes %XX, unlike RFC 3986 which leaves the slash in path segments.
    std::string aws_uri_encode(std::string_view value, bool encode_slash)
    {
        static constexpr char DIGITS[] = "0123456789ABCDEF";
        std::string out;
        out.reserve(value.size());
        for (const char c : value) {
            const unsigned char u = static_cast<unsigned char>(c);
            const bool safe = (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z')
                || (u >= '0' && u <= '9') || u == '-' || u == '.' || u == '_'
                || u == '~' || (u == '/' && !encode_slash);
            if (safe) {
                out.push_back(c);
            } else {
                out.push_back('%');
                out.push_back(DIGITS[u >> 4]);
                out.push_back(DIGITS[u & 0xf]);
            }
        }
        return out;
    }

    struct ParsedUrl {
        std::string host;
        std::string path;  // begins with '/', aws_uri_encode(false)-ready
        std::string query; // without '?'
    };

    ParsedUrl parse_url(const std::string& url)
    {
        ParsedUrl parsed;
        std::string rest = url;
        if (const auto scheme = rest.find("://"); scheme != std::string::npos) {
            rest = rest.substr(scheme + 3);
        }
        const auto slash = rest.find('/');
        parsed.host = slash == std::string::npos ? rest : rest.substr(0, slash);
        if (slash == std::string::npos) {
            parsed.path = "/";
            return parsed;
        }
        const std::string path_and_query = rest.substr(slash);
        const auto question              = path_and_query.find('?');
        parsed.path                      = question == std::string::npos
            ? path_and_query
            : path_and_query.substr(0, question);
        if (question != std::string::npos) {
            parsed.query = path_and_query.substr(question + 1);
        }
        return parsed;
    }

    // Region from a bedrock-runtime host: the first label after the
    // prefix (bedrock-runtime.eu-west-1.amazonaws.com -> eu-west-1).
    std::string region_from_host(const std::string& host)
    {
        static const std::string PREFIX = "bedrock-runtime.";
        if (host.rfind(PREFIX, 0) != 0) {
            return env_or_empty("AWS_REGION");
        }
        std::string rest = host.substr(PREFIX.size());
        if (const auto dot = rest.find('.'); dot != std::string::npos) {
            rest = rest.substr(0, dot);
        }
        return rest;
    }

    std::mutex& credential_mutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    AwsCredentials& cached_credentials()
    {
        static AwsCredentials cached;
        return cached;
    }

    std::int64_t& cached_expiry()
    {
        static std::int64_t expiry = 0;
        return expiry;
    }

    std::int64_t now_unix()
    {
        // Test seam: deterministic SigV4 vectors need a frozen clock.
        const std::string fake = env_or_empty("IMZA_FAKE_UNIX_TIME");
        if (!fake.empty()) {
            return std::atoll(fake.c_str());
        }
        return static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    }

    // Consider a session token expired five minutes early, so a signing
    // that lands near the boundary cannot reach AWS with a stale token.
    constexpr std::int64_t EXPIRY_SLACK_SECS = 300;
    constexpr const char* STS_HOST           = "sts.amazonaws.com";
    constexpr const char* CONTAINER_HOST     = "169.254.170.2";

    // AssumeRoleWithWebIdentity is an unsigned HTTPS POST: the JWT is a
    // form parameter, so the IRSA exchange needs no SigV4 machinery.
    bool assume_role_with_web_identity(const std::string& token_file,
        const std::string& role_arn, AwsCredentials& out, std::string* message)
    {
        std::FILE* file = std::fopen(token_file.c_str(), "rb");
        if (file == nullptr) {
            if (message != nullptr) {
                *message = "Cannot read AWS_WEB_IDENTITY_TOKEN_FILE.";
            }
            return false;
        }
        std::string token;
        char buffer[4096];
        std::size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
            token.append(buffer, read);
        }
        std::fclose(file);
        while (!token.empty()
            && (token.back() == '\n' || token.back() == '\r'
                || token.back() == ' ')) {
            token.pop_back();
        }
        if (token.empty()) {
            if (message != nullptr) {
                *message = "AWS web identity token file is empty.";
            }
            return false;
        }

        std::string body = "Action=AssumeRoleWithWebIdentity&Version=2011-06-15"
                           "&RoleArn=";
        body += aws_uri_encode(role_arn, true);
        body += "&RoleSessionName=imza-session&WebIdentityToken=";
        body += aws_uri_encode(token, true);

        // Test seam: the IRSA exchange points at sts.amazonaws.com; loopback
        // tests redirect it to a local fixture server.
        const std::string override_endpoint = env_or_empty("IMZA_TEST_STS_URL");
        const std::string url               = override_endpoint.empty()
            ? std::string("https://") + STS_HOST + "/"
            : override_endpoint;
        std::string response;
        long code           = 0;
        const Status status = http_post(url,
            { "Content-Type: application/x-www-form-urlencoded; charset=utf-8",
                "Accept: application/json" },
            body, 30, response, &code);
        if (status != Status::OK || !http_ok(code)) {
            if (message != nullptr) {
                *message = "STS AssumeRoleWithWebIdentity failed (HTTP "
                    + std::to_string(code) + ").";
            }
            return false;
        }
        const JsonValue root = parse_json(response);
        const auto text_of   = [](const JsonValue* v) -> std::string {
            return v != nullptr && v->is_string() ? v->as<std::string>() : "";
        };
        const JsonValue* result
            = find_member(root, "AssumeRoleWithWebIdentityResponse");
        if (result == nullptr) {
            if (message != nullptr) {
                *message
                    = "STS response missing AssumeRoleWithWebIdentityResponse.";
            }
            return false;
        }
        const JsonValue* inner
            = find_member(*result, "AssumeRoleWithWebIdentityResult");
        const JsonValue* creds
            = inner != nullptr ? find_member(*inner, "Credentials") : nullptr;
        if (creds == nullptr) {
            if (message != nullptr) {
                *message = "STS response missing Credentials.";
            }
            return false;
        }
        out.access_key    = text_of(find_member(*creds, "AccessKeyId"));
        out.secret_key    = text_of(find_member(*creds, "SecretAccessKey"));
        out.session_token = text_of(find_member(*creds, "SessionToken"));
        const std::string expiration
            = text_of(find_member(*creds, "Expiration"));
        // STS Expiration is ISO 8601 ("2026-01-01T00:00:00Z").
        std::tm tm { };
        if (out.access_key.empty() || out.secret_key.empty()
            || expiration.size() < 19
            || std::sscanf(expiration.c_str(), "%d-%d-%dT%d:%d:%d", &tm.tm_year,
                   &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec)
                != 6) {
            if (message != nullptr) {
                *message = "STS response missing usable credentials.";
            }
            return false;
        }
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        tm.tm_isdst = 0;
#ifdef _WIN32
        std::int64_t expires = static_cast<std::int64_t>(_mkgmtime(&tm));
#else
        std::int64_t expires = static_cast<std::int64_t>(timegm(&tm));
#endif
        if (expires <= 0) {
            if (message != nullptr) {
                *message = "STS Expiration is unparseable.";
            }
            return false;
        }
        cached_expiry() = expires;
        return true;
    }

    // ECS/EKS agent endpoint: plain GET returning a JSON credential doc.
    bool container_credentials(AwsCredentials& out, std::string* message)
    {
        const std::string path
            = env_or_empty("AWS_CONTAINER_CREDENTIALS_RELATIVE_URI");
        std::string response;
        long code = 0;
        const Status status
            = http_get(std::string("http://") + CONTAINER_HOST + path, { }, 10,
                response, &code);
        if (status != Status::OK || !http_ok(code)) {
            if (message != nullptr) {
                *message = "Container credentials endpoint failed (HTTP "
                    + std::to_string(code) + ").";
            }
            return false;
        }
        const JsonValue root = parse_json(response);
        const auto text_of   = [](const JsonValue* v) -> std::string {
            return v != nullptr && v->is_string() ? v->as<std::string>() : "";
        };
        out.access_key    = text_of(find_member(root, "AccessKeyId"));
        out.secret_key    = text_of(find_member(root, "SecretAccessKey"));
        out.session_token = text_of(find_member(root, "Token"));
        if (out.access_key.empty() || out.secret_key.empty()) {
            if (message != nullptr) {
                *message = "Container credentials response missing keys.";
            }
            return false;
        }
        // The agent refreshes credentials in the background; treat them as
        // an hour so we re-read rather than holding them forever.
        cached_expiry() = now_unix() + 3600;
        return true;
    }

} // namespace

std::vector<std::string> sigv4_headers(const AwsCredentials& credentials,
    const std::string& method, const std::string& url,
    const std::string& payload)
{
    const ParsedUrl parsed = parse_url(url);

    // x-amz-date is "YYYYMMDDTHHMMSSZ"; the scope date is its date part.
    const std::time_t now = static_cast<std::time_t>(now_unix());
    std::tm utc { };
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char amz_date[20] { };
    std::strftime(amz_date, sizeof(amz_date), "%Y%m%dT%H%M%SZ", &utc);
    char date_stamp[9] { };
    std::strftime(date_stamp, sizeof(date_stamp), "%Y%m%d", &utc);

    const std::string region  = region_from_host(parsed.host);
    const std::string service = "bedrock";

    // Canonical headers: host plus every signed header we send, lowercased,
    // trimmed, one per line, sorted; the signed list names them in order.
    // Only the headers we actually sign may vary per request, so the set is
    // fixed: host, x-amz-date, x-amz-security-token (when present).
    std::string canonical_headers = "host:" + parsed.host + "\n";
    canonical_headers += "x-amz-date:";
    canonical_headers += amz_date;
    canonical_headers += "\n";
    std::string signed_headers = "host;x-amz-date";
    if (!credentials.session_token.empty()) {
        canonical_headers += "x-amz-security-token:";
        canonical_headers += credentials.session_token;
        canonical_headers += "\n";
        signed_headers += ";x-amz-security-token";
    }

    const auto payload_hash = hex(sha256(payload));

    std::string canonical_request;
    canonical_request += method + "\n";
    canonical_request += aws_uri_encode(parsed.path, false) + "\n";
    canonical_request += parsed.query + "\n";
    canonical_request += canonical_headers + "\n";
    canonical_request += signed_headers + "\n";
    canonical_request += payload_hash;

    const std::string scope = std::string(date_stamp) + "/" + region + "/"
        + service + "/aws4_request";
    std::string string_to_sign;
    string_to_sign += "AWS4-HMAC-SHA256\n";
    string_to_sign += std::string(amz_date) + "\n";
    string_to_sign += scope + "\n";
    string_to_sign += hex(sha256(canonical_request));

    const std::string k_secret = "AWS4" + credentials.secret_key;
    const auto k_date          = hmac_sha256(k_secret, date_stamp);
    const auto k_region        = hmac_sha256(
        std::string_view(
            reinterpret_cast<const char*>(k_date.data()), k_date.size()),
        region);
    const auto k_service = hmac_sha256(
        std::string_view(
            reinterpret_cast<const char*>(k_region.data()), k_region.size()),
        service);
    const auto k_signing = hmac_sha256(
        std::string_view(
            reinterpret_cast<const char*>(k_service.data()), k_service.size()),
        "aws4_request");
    const auto signature = hmac_sha256(
        std::string_view(
            reinterpret_cast<const char*>(k_signing.data()), k_signing.size()),
        string_to_sign);

    std::vector<std::string> out;
    out.push_back(std::string("x-amz-date: ") + amz_date);
    if (!credentials.session_token.empty()) {
        out.push_back("x-amz-security-token: " + credentials.session_token);
    }
    out.push_back("Authorization: AWS4-HMAC-SHA256 Credential="
        + credentials.access_key + "/" + scope + ", SignedHeaders="
        + signed_headers + ", Signature=" + hex(signature));
    return out;
}

Status resolve_aws_credentials(AwsCredentials& out, std::string* message)
{
    std::lock_guard lock(credential_mutex());

    const std::int64_t now = now_unix();
    if (!cached_credentials().access_key.empty()
        && (cached_expiry() == 0
            || now < cached_expiry() - EXPIRY_SLACK_SECS)) {
        out = cached_credentials();
        return Status::OK;
    }

    const std::string access = env_or_empty("AWS_ACCESS_KEY_ID");
    const std::string secret = env_or_empty("AWS_SECRET_ACCESS_KEY");
    if (!access.empty() && !secret.empty()) {
        cached_credentials()
            = { access, secret, env_or_empty("AWS_SESSION_TOKEN") };
        cached_expiry() = 0;
        out             = cached_credentials();
        return Status::OK;
    }

    const std::string token_file = env_or_empty("AWS_WEB_IDENTITY_TOKEN_FILE");
    const std::string role_arn   = env_or_empty("AWS_ROLE_ARN");
    if (!token_file.empty() && !role_arn.empty()) {
        AwsCredentials resolved;
        if (assume_role_with_web_identity(
                token_file, role_arn, resolved, message)) {
            cached_credentials() = resolved;
            out                  = resolved;
            return Status::OK;
        }
        return Status::CONFIG_ERROR;
    }

    const std::string relative
        = env_or_empty("AWS_CONTAINER_CREDENTIALS_RELATIVE_URI");
    if (!relative.empty()) {
        AwsCredentials resolved;
        if (container_credentials(resolved, message)) {
            cached_credentials() = resolved;
            out                  = resolved;
            return Status::OK;
        }
        return Status::CONFIG_ERROR;
    }

    if (message != nullptr) {
        *message = "No AWS credentials found. Export AWS_ACCESS_KEY_ID and"
                   " AWS_SECRET_ACCESS_KEY, or run inside an IRSA-equipped"
                   " pod or task role.";
    }
    return Status::CONFIG_ERROR;
}

void invalidate_aws_credentials()
{
    std::lock_guard lock(credential_mutex());
    cached_credentials() = { };
    cached_expiry()      = 0;
}

} // namespace imza
