#include "providers/subscriptions.h"

#include "network/json_io.h"
#include "network/network.h"
#include "providers/catalog.h"

#include <json/json.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <mutex>
#include <string_view>
#include <thread>

namespace imza {

namespace {

    constexpr std::string_view OPENAI_CLIENT_ID
        = "app_EMoamEEZ73f0CkXaXp7hrann";
    constexpr std::string_view ANTHROPIC_CLIENT_ID
        = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
    constexpr std::string_view ANTHROPIC_REDIRECT
        = "https://platform.claude.com/oauth/code/callback";

    SubscriptionHttpPost http_post_fn(SubscriptionHttpPost post)
    {
        if (post) {
            return post;
        }
        return
            [](const std::string& url, const std::vector<std::string>& headers,
                const std::string& payload, long timeout, std::string& body,
                long* code) {
                return http_post(url, headers, payload, timeout, body, code, 0);
            };
    }

    SubscriptionResult failure(Status status, std::string body)
    {
        return { status, { },
            body.empty() ? error_text(status) : std::move(body) };
    }

    std::string base64url(const unsigned char* bytes, std::size_t size)
    {
        constexpr std::string_view alphabet
            = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-"
              "_";
        std::string out;
        out.reserve((size * 4 + 2) / 3);
        std::uint32_t value = 0;
        int bits            = -6;
        for (std::size_t i = 0; i < size; ++i) {
            value = (value << 8) | bytes[i];
            bits += 8;
            while (bits >= 0) {
                out.push_back(alphabet[(value >> bits) & 0x3f]);
                bits -= 6;
            }
        }
        if (bits > -6) {
            out.push_back(alphabet[(value << 8 >> (bits + 8)) & 0x3f]);
        }
        return out;
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

    std::string url_encode(std::string_view value)
    {
        constexpr char hex[] = "0123456789ABCDEF";
        std::string out;
        for (const unsigned char c : value) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'
                || c == '~') {
                out.push_back(static_cast<char>(c));
            } else {
                out.push_back('%');
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 0xf]);
            }
        }
        return out;
    }

    std::string url_decode(std::string_view value)
    {
        std::string out;
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '+') {
                out.push_back(' ');
                continue;
            }
            if (value[i] != '%' || i + 2 >= value.size()) {
                out.push_back(value[i]);
                continue;
            }
            unsigned decoded  = 0;
            const auto result = std::from_chars(
                value.data() + i + 1, value.data() + i + 3, decoded, 16);
            if (result.ec != std::errc()) {
                out.push_back(value[i]);
                continue;
            }
            out.push_back(static_cast<char>(decoded));
            i += 2;
        }
        return out;
    }

    std::string query_value(std::string_view text, std::string_view key)
    {
        const std::size_t query = text.find('?');
        if (query != std::string_view::npos) {
            text.remove_prefix(query + 1);
        }
        const std::size_t fragment = text.find('#');
        if (fragment != std::string_view::npos) {
            text = text.substr(0, fragment);
        }
        while (!text.empty()) {
            const std::size_t amp       = text.find('&');
            const std::string_view item = text.substr(0, amp);
            const std::size_t equal     = item.find('=');
            if (equal != std::string_view::npos
                && item.substr(0, equal) == key) {
                return url_decode(item.substr(equal + 1));
            }
            if (amp == std::string_view::npos) {
                break;
            }
            text.remove_prefix(amp + 1);
        }
        return { };
    }

    bool wait_default(std::stop_token stop, std::chrono::seconds duration)
    {
        std::mutex mutex;
        std::condition_variable_any changed;
        std::unique_lock lock(mutex);
        return !changed.wait_for(lock, stop, duration, [] { return false; });
    }

    std::int64_t expires_from(
        const Json::Value& root, std::string_view access_token)
    {
        if (root["expires_in"].isInt64()) {
            return static_cast<std::int64_t>(std::time(nullptr))
                + root["expires_in"].asInt64();
        }
        std::string account;
        std::string label;
        std::int64_t expires = 0;
        parse_openai_token_claims(access_token, account, label, expires);
        return expires;
    }

    SubscriptionResult parse_token_response(const std::string& body,
        bool openai, std::string_view old_refresh = { },
        std::string_view old_account = { })
    {
        const Json::Value root = parse_json(body);
        if (!root.isObject() || !root["access_token"].isString()
            || root["access_token"].asString().empty()) {
            return failure(Status::JSON_ERROR, body);
        }
        SubscriptionCredentials credentials;
        credentials.access_token  = root["access_token"].asString();
        credentials.refresh_token = root["refresh_token"].isString()
            ? root["refresh_token"].asString()
            : std::string(old_refresh);
        credentials.expires_at = expires_from(root, credentials.access_token);
        credentials.account_id = std::string(old_account);
        if (openai) {
            const std::string token = root["id_token"].isString()
                ? root["id_token"].asString()
                : credentials.access_token;
            std::int64_t ignored    = 0;
            parse_openai_token_claims(
                token, credentials.account_id, credentials.label, ignored);
        }
        if (credentials.refresh_token.empty()
            || (openai && credentials.account_id.empty())) {
            return failure(Status::JSON_ERROR, body);
        }
        return { Status::OK, std::move(credentials), { } };
    }

    SubscriptionResult exchange_openai(
        const Json::Value& authorization, const SubscriptionHttpPost& post)
    {
        if (!authorization["authorization_code"].isString()
            || !authorization["code_verifier"].isString()) {
            return failure(Status::JSON_ERROR, authorization.toStyledString());
        }
        const std::string payload = "grant_type=authorization_code&code="
            + url_encode(authorization["authorization_code"].asString())
            + "&redirect_uri="
            + url_encode("https://auth.openai.com/deviceauth/callback")
            + "&client_id=" + url_encode(OPENAI_CLIENT_ID) + "&code_verifier="
            + url_encode(authorization["code_verifier"].asString());
        std::string body;
        long code           = 0;
        const Status status = post("https://auth.openai.com/oauth/token",
            { "Content-Type: application/x-www-form-urlencoded" }, payload, 30,
            body, &code);
        if (status != Status::OK) {
            return failure(status, body);
        }
        if (code < 200 || code >= 300) {
            return failure(Status::API_ERROR, body);
        }
        return parse_token_response(body, true);
    }

} // namespace

std::string pkce_challenge(std::string_view verifier)
{
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest { };
    SHA256(reinterpret_cast<const unsigned char*>(verifier.data()),
        verifier.size(), digest.data());
    return base64url(digest.data(), digest.size());
}

bool parse_openai_token_claims(std::string_view token, std::string& account_id,
    std::string& label, std::int64_t& expires_at)
{
    const std::size_t first  = token.find('.');
    const std::size_t second = first == std::string_view::npos
        ? std::string_view::npos
        : token.find('.', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos) {
        return false;
    }
    const Json::Value root = parse_json(
        decode_base64url(token.substr(first + 1, second - first - 1)));
    if (!root.isObject()) {
        return false;
    }
    constexpr std::string_view claim = "https://api.openai.com/auth";
    const Json::Value& auth          = root[std::string(claim)];
    if (auth.isObject()) {
        if (auth["chatgpt_account_id"].isString()) {
            account_id = auth["chatgpt_account_id"].asString();
        }
        if (auth["user_email"].isString()) {
            label = auth["user_email"].asString();
        } else if (auth["chatgpt_plan_type"].isString()) {
            label = auth["chatgpt_plan_type"].asString();
        }
    }
    if (root["exp"].isInt64()) {
        expires_at = root["exp"].asInt64();
    }
    return !account_id.empty();
}

OpenAIDeviceCodeResult request_openai_device_code(SubscriptionHttpPost post)
{
    post = http_post_fn(std::move(post));
    Json::Value request(Json::objectValue);
    request["client_id"] = std::string(OPENAI_CLIENT_ID);
    std::string body;
    long code = 0;
    const Status status
        = post("https://auth.openai.com/api/accounts/deviceauth/usercode",
            { "Content-Type: application/json" }, write_json(request), 30, body,
            &code);
    if (status != Status::OK) {
        return { status, { }, body.empty() ? error_text(status) : body };
    }
    if (code < 200 || code >= 300) {
        return { Status::API_ERROR, { }, body };
    }
    const Json::Value root = parse_json(body);
    if (!root.isObject() || !root["device_auth_id"].isString()
        || !root["user_code"].isString()) {
        return { Status::JSON_ERROR, { }, body };
    }
    long interval = 5;
    if (root["interval"].isString()) {
        const std::string interval_text = root["interval"].asString();
        std::from_chars(interval_text.data(),
            interval_text.data() + interval_text.size(), interval);
    } else if (root["interval"].isInt()) {
        interval = root["interval"].asInt();
    }
    interval = std::clamp(interval, 1L, 60L);
    return { Status::OK,
        { root["device_auth_id"].asString(), root["user_code"].asString(),
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
    Json::Value request(Json::objectValue);
    request["device_auth_id"] = code.device_auth_id;
    request["user_code"]      = code.user_code;
    std::chrono::seconds elapsed { 0 };
    while (!stop.stop_requested() && elapsed < std::chrono::minutes(15)) {
        std::string body;
        long http_code = 0;
        const Status status
            = post("https://auth.openai.com/api/accounts/deviceauth/token",
                { "Content-Type: application/json" }, write_json(request), 30,
                body, &http_code);
        if (status != Status::OK) {
            return failure(status, body);
        }
        if (http_code >= 200 && http_code < 300) {
            return exchange_openai(parse_json(body), post);
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

AnthropicAuthorizationResult make_anthropic_authorization()
{
    std::array<unsigned char, 32> verifier_bytes { };
    std::array<unsigned char, 24> state_bytes { };
    if (RAND_bytes(verifier_bytes.data(), verifier_bytes.size()) != 1
        || RAND_bytes(state_bytes.data(), state_bytes.size()) != 1) {
        return { Status::API_ERROR, { }, "Failed to generate sign-in code." };
    }
    AnthropicAuthorization authorization;
    authorization.verifier
        = base64url(verifier_bytes.data(), verifier_bytes.size());
    authorization.state = base64url(state_bytes.data(), state_bytes.size());
    authorization.url
        = "https://claude.ai/oauth/authorize?code=true&response_type=code"
          "&client_id="
        + url_encode(ANTHROPIC_CLIENT_ID)
        + "&redirect_uri=" + url_encode(ANTHROPIC_REDIRECT) + "&scope="
        + url_encode("org:create_api_key user:profile user:inference")
        + "&code_challenge="
        + url_encode(pkce_challenge(authorization.verifier))
        + "&code_challenge_method=S256&state="
        + url_encode(authorization.state);
    return { Status::OK, std::move(authorization), { } };
}

SubscriptionResult exchange_anthropic_code(
    const AnthropicAuthorization& authorization, std::string_view pasted,
    SubscriptionHttpPost post)
{
    std::string code;
    std::string state;
    if (pasted.find('?') != std::string_view::npos) {
        code  = query_value(pasted, "code");
        state = query_value(pasted, "state");
    } else if (const std::size_t split = pasted.find('#');
        split != std::string_view::npos) {
        code  = std::string(pasted.substr(0, split));
        state = std::string(pasted.substr(split + 1));
    } else {
        code = std::string(pasted);
    }
    if (code.empty()) {
        return failure(Status::API_ERROR, "Authorization code is empty.");
    }
    if (!state.empty() && state != authorization.state) {
        return failure(
            Status::API_ERROR, "Authorization state does not match.");
    }
    Json::Value request(Json::objectValue);
    request["grant_type"]    = "authorization_code";
    request["client_id"]     = std::string(ANTHROPIC_CLIENT_ID);
    request["code"]          = std::move(code);
    request["state"]         = authorization.state;
    request["redirect_uri"]  = std::string(ANTHROPIC_REDIRECT);
    request["code_verifier"] = authorization.verifier;
    std::string body;
    long http_code      = 0;
    post                = http_post_fn(std::move(post));
    const Status status = post("https://platform.claude.com/v1/oauth/token",
        { "Content-Type: application/json" }, write_json(request), 30, body,
        &http_code);
    if (status != Status::OK) {
        return failure(status, body);
    }
    if (http_code < 200 || http_code >= 300) {
        return failure(Status::API_ERROR, body);
    }
    return parse_token_response(body, false);
}

SubscriptionResult refresh_subscription(std::string_view connection_id,
    std::string_view refresh_token, std::string_view account_id,
    SubscriptionHttpPost post)
{
    Json::Value request(Json::objectValue);
    request["grant_type"]    = "refresh_token";
    request["refresh_token"] = std::string(refresh_token);
    std::string url;
    bool openai = false;
    if (connection_id == OPENAI_SUBSCRIPTION_ID) {
        openai               = true;
        url                  = "https://auth.openai.com/oauth/token";
        request["client_id"] = std::string(OPENAI_CLIENT_ID);
    } else if (connection_id == ANTHROPIC_SUBSCRIPTION_ID) {
        url                  = "https://platform.claude.com/v1/oauth/token";
        request["client_id"] = std::string(ANTHROPIC_CLIENT_ID);
    } else {
        return failure(Status::API_ERROR, "Not a subscription connection.");
    }
    std::string body;
    long http_code      = 0;
    post                = http_post_fn(std::move(post));
    const Status status = post(url, { "Content-Type: application/json" },
        write_json(request), 30, body, &http_code);
    if (status != Status::OK) {
        return failure(status, body);
    }
    if (http_code < 200 || http_code >= 300) {
        return failure(Status::API_ERROR, body);
    }
    return parse_token_response(body, openai, refresh_token, account_id);
}

} // namespace imza
