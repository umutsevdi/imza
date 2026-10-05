#include "providers/subscriptions.h"

#include "common/util.h"
#include "network/json.h"
#include "network/network.h"
#include "providers/catalog.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <mutex>
#include <string_view>

namespace imza {

namespace {

    constexpr std::string_view OPENAI_CLIENT_ID
        = "app_EMoamEEZ73f0CkXaXp7hrann";

    SubscriptionHttpPost http_post_fn(SubscriptionHttpPost post)
    {
        if (post) {
            return post;
        }
        return [](const std::string& url,
                   const std::vector<std::string>& headers,
                   const std::string& payload, long timeout, std::string& body,
                   long* code) {
            HttpPostOptions opts { };
            opts.max_redirs = 0;
            return http_post(url, headers, payload, timeout, body, code, opts);
        };
    }

    SubscriptionResult failure(Status status, std::string body)
    {
        return { status, { },
            body.empty() ? error_text(status) : std::move(body) };
    }

    std::string decode_base64url(std::string_view input)
    {
        std::array<int, 256> values;
        values.fill(-1);
        constexpr std::string_view alphabet
            = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-"
              "_";
        for (std::size_t i = 0; i < alphabet.size(); ++i) {
            values[static_cast<unsigned char>(alphabet[i])]
                = static_cast<int>(i);
        }
        std::string out;
        std::uint32_t value = 0;
        int bits            = -8;
        for (const unsigned char c : input) {
            if (values[c] < 0) {
                return { };
            }
            value = (value << 6) | static_cast<std::uint32_t>(values[c]);
            bits += 6;
            if (bits >= 0) {
                out.push_back(static_cast<char>((value >> bits) & 0xff));
                bits -= 8;
            }
        }
        return out;
    }

    bool wait_default(std::stop_token stop, std::chrono::seconds duration)
    {
        std::mutex mutex;
        std::condition_variable_any changed;
        std::unique_lock lock(mutex);
        return !changed.wait_for(lock, stop, duration, [] { return false; });
    }

    // OAuth request/response wire shapes. File scope: Glaze reflection
    // rejects function-local types.
    struct ClientIdRequest {
        std::string client_id;
    };

    struct DeviceTokenRequest {
        std::string device_auth_id;
        std::string user_code;
    };

    struct RefreshRequest {
        std::string grant_type = "refresh_token";
        std::string refresh_token;
        std::string client_id;
    };

    // OAuth response wire shapes.
    struct StoredTokenResponse {
        std::optional<std::string> access_token;
        std::optional<std::string> refresh_token;
        std::optional<std::string> id_token;
        std::optional<std::int64_t> expires_in;
    };

    struct StoredDeviceCodeResponse {
        std::optional<std::string> device_auth_id;
        std::optional<std::string> user_code;
        std::optional<std::string> interval;
        std::optional<std::string> verification_uri;
        std::optional<std::string> verification_uri_complete;
    };

    std::int64_t expires_from(
        const StoredTokenResponse& root, std::string_view access_token)
    {
        if (root.expires_in) {
            return static_cast<std::int64_t>(std::time(nullptr))
                + *root.expires_in;
        }
        std::string account;
        std::int64_t expires = 0;
        parse_openai_token_claims(access_token, account, expires);
        return expires;
    }

    SubscriptionResult parse_token_response(const std::string& body,
        std::string_view old_refresh = { }, std::string_view old_account = { })
    {
        StoredTokenResponse root;
        if (json_parse_checked(body, root) || !root.access_token
            || root.access_token->empty()) {
            return failure(Status::JSON_ERROR, body);
        }
        SubscriptionCredentials credentials;
        credentials.access_token = *root.access_token;
        credentials.refresh_token
            = root.refresh_token.value_or(std::string(old_refresh));
        credentials.expires_at = expires_from(root, credentials.access_token);
        credentials.account_id = std::string(old_account);
        std::string token      = credentials.access_token;
        if (root.id_token) {
            token = *root.id_token;
        }
        std::int64_t ignored = 0;
        parse_openai_token_claims(token, credentials.account_id, ignored);
        if (credentials.refresh_token.empty()
            || credentials.account_id.empty()) {
            return failure(Status::JSON_ERROR, body);
        }
        return { Status::OK, std::move(credentials), { } };
    }

    struct StoredAuthorization {
        std::optional<std::string> authorization_code;
        std::optional<std::string> code_verifier;
    };

    SubscriptionResult exchange_openai(const StoredAuthorization& authorization,
        const SubscriptionHttpPost& post)
    {
        if (!authorization.authorization_code || !authorization.code_verifier) {
            return failure(Status::JSON_ERROR, json_dump(authorization));
        }
        const std::string payload = "grant_type=authorization_code&code="
            + percent_encode(*authorization.authorization_code)
            + "&redirect_uri="
            + percent_encode("https://auth.openai.com/deviceauth/callback")
            + "&client_id=" + percent_encode(OPENAI_CLIENT_ID)
            + "&code_verifier=" + percent_encode(*authorization.code_verifier);
        std::string body;
        long code           = 0;
        const Status status = post("https://auth.openai.com/oauth/token",
            { "Content-Type: application/x-www-form-urlencoded" }, payload, 30,
            body, &code);
        if (status != Status::OK) {
            return failure(status, body);
        }
        if (!http_ok(code)) {
            return failure(Status::API_ERROR, body);
        }
        return parse_token_response(body);
    }

} // namespace

bool parse_openai_token_claims(
    std::string_view token, std::string& account_id, std::int64_t& expires_at)
{
    const std::size_t first  = token.find('.');
    const std::size_t second = first == std::string_view::npos
        ? std::string_view::npos
        : token.find('.', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos) {
        return false;
    }
    JsonValue root;
    if (!json_parse(
            decode_base64url(token.substr(first + 1, second - first - 1)), root)
        || !root.is_object()) {
        return false;
    }
    constexpr std::string_view claim = "https://api.openai.com/auth";
    if (const JsonValue* auth = find_member(root, claim);
        auth != nullptr && auth->is_object()) {
        if (const JsonValue* id = find_member(*auth, "chatgpt_account_id");
            id != nullptr && id->is_string()) {
            account_id = id->as<std::string>();
        }
    }
    if (const JsonValue* exp = find_member(root, "exp");
        exp != nullptr && exp->is_number()) {
        expires_at = exp->as<std::int64_t>();
    }
    return !account_id.empty();
}

OpenAIDeviceCodeResult request_openai_device_code(SubscriptionHttpPost post)
{
    post = http_post_fn(std::move(post));
    const ClientIdRequest request { std::string(OPENAI_CLIENT_ID) };
    std::string body;
    long code = 0;
    const Status status
        = post("https://auth.openai.com/api/accounts/deviceauth/usercode",
            { "Content-Type: application/json" }, json_dump(request), 30, body,
            &code);
    if (status != Status::OK) {
        return { status, { }, body.empty() ? error_text(status) : body };
    }
    if (!http_ok(code)) {
        return { Status::API_ERROR, { }, body };
    }
    StoredDeviceCodeResponse root;
    if (!json_parse_checked(body, root) || !root.device_auth_id
        || !root.user_code) {
        return { Status::JSON_ERROR, { }, body };
    }
    // The interval arrives either as a number or a numeric string.
    long interval = 5;
    if (root.interval) {
        std::from_chars(root.interval->data(),
            root.interval->data() + root.interval->size(), interval);
    }
    interval = std::clamp(interval, 1L, 60L);
    return { Status::OK,
        { *root.device_auth_id, *root.user_code,
            "https://auth.openai.com/codex/device",
            std::chrono::seconds(interval) },
        { } };
}

SubscriptionResult await_openai_device_code(const OpenAIDeviceCode& code,
    std::stop_token stop, SubscriptionHttpPost post, SubscriptionWait wait)
{
    post = http_post_fn(std::move(post));
    if (!wait) {
        wait = wait_default;
    }
    const DeviceTokenRequest request { code.device_auth_id, code.user_code };
    std::chrono::seconds elapsed { 0 };
    while (!stop.stop_requested() && elapsed < std::chrono::minutes(15)) {
        std::string body;
        long http_code = 0;
        const Status status
            = post("https://auth.openai.com/api/accounts/deviceauth/token",
                { "Content-Type: application/json" }, json_dump(request), 30,
                body, &http_code);
        if (status != Status::OK) {
            return failure(status, body);
        }
        if (http_ok(http_code)) {
            StoredAuthorization payload;
            if (json_parse_checked(body, payload)) {
                return failure(Status::JSON_ERROR, body);
            }
            return exchange_openai(payload, post);
        }
        if (http_code != 403 && http_code != 404) {
            return failure(Status::API_ERROR, body);
        }
        if (!wait(stop, code.interval)) {
            return failure(Status::CANCELLED, { });
        }
        elapsed += code.interval;
    }
    return failure(
        stop.stop_requested() ? Status::CANCELLED : Status::TIMEOUT, { });
}

SubscriptionResult refresh_subscription(std::string_view connection_id,
    std::string_view refresh_token, std::string_view account_id,
    SubscriptionHttpPost post)
{
    if (connection_id != OPENAI_SUBSCRIPTION_ID) {
        return failure(Status::API_ERROR, "Not a subscription connection.");
    }
    const RefreshRequest request { { }, std::string(refresh_token),
        std::string(OPENAI_CLIENT_ID) };
    std::string body;
    long http_code      = 0;
    post                = http_post_fn(std::move(post));
    const Status status = post("https://auth.openai.com/oauth/token",
        { "Content-Type: application/json" }, json_dump(request), 30, body,
        &http_code);
    if (status != Status::OK) {
        return failure(status, body);
    }
    if (!http_ok(http_code)) {
        return failure(Status::API_ERROR, body);
    }
    return parse_token_response(body, refresh_token, account_id);
}

} // namespace imza
