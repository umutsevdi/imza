#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "common/types.h"

namespace imza {

using SubscriptionHttpPost
    = std::function<Status(const std::string&, const std::vector<std::string>&,
        const std::string&, long, std::string&, long*)>;
using SubscriptionWait
    = std::function<bool(std::stop_token, std::chrono::seconds)>;

struct SubscriptionCredentials {
    std::string access_token;
    std::string refresh_token;
    std::int64_t expires_at = 0;
    std::string account_id;
    std::string label;
};

struct SubscriptionResult {
    Status status = Status::OK;
    SubscriptionCredentials credentials;
    std::string error;
};

struct OpenAIDeviceCode {
    std::string device_auth_id;
    std::string user_code;
    std::string verification_url;
    std::chrono::seconds interval { 5 };
};

struct OpenAIDeviceCodeResult {
    Status status = Status::OK;
    OpenAIDeviceCode code;
    std::string error;
};

OpenAIDeviceCodeResult request_openai_device_code(
    SubscriptionHttpPost post = { });
SubscriptionResult await_openai_device_code(const OpenAIDeviceCode& code,
    std::stop_token stop, SubscriptionHttpPost post = { },
    SubscriptionWait wait = { });
SubscriptionResult refresh_subscription(std::string_view connection_id,
    std::string_view refresh_token, std::string_view account_id = { },
    SubscriptionHttpPost post = { });
bool parse_openai_token_claims(std::string_view token, std::string& account_id,
    std::string& label, std::int64_t& expires_at);

} // namespace imza
