#include <doctest/doctest.h>

#include "network/json_io.h"
#include "providers/catalog.h"
#include "providers/subscriptions.h"

namespace {

std::string token_with_claims()
{
    return "e30.eyJleHAiOjE3NTYzOTAwMDAsImh0dHBzOi8vYXBpLm9wZW5haS5jb20v"
           "YXV0aCI6eyJjaGF0Z3B0X2FjY291bnRfaWQiOiJhY2NvdW50LTEiLCJ1c2VyX2"
           "VtYWlsIjoidXNlckBleGFtcGxlLmNvbSJ9fQ.signature";
}

} // namespace

TEST_CASE("OpenAI token claims provide account identity and expiry")
{
    std::string account;
    std::int64_t expires = 0;
    REQUIRE(
        imza::parse_openai_token_claims(token_with_claims(), account, expires));
    CHECK(account == "account-1");
    CHECK(expires == 1756390000);
}

TEST_CASE("subscription refresh posts a refresh_token grant")
{
    std::string seen_body;
    const auto post
        = [&](const std::string& url, const std::vector<std::string>&,
              const std::string& body, long, std::string& out, long* code) {
              CHECK(url.ends_with("/oauth/token"));
              seen_body = body;
              *code     = 200;
              out       = R"({"access_token":"a","refresh_token":"r"})";
              return imza::Status::OK;
          };
    const auto result = imza::refresh_subscription(
        imza::OPENAI_SUBSCRIPTION_ID, "refresh-token", "account-1", post);
    CHECK(result.status == imza::Status::OK);
    CHECK(
        seen_body.find(R"("grant_type":"refresh_token")") != std::string::npos);
    CHECK(seen_body.find(R"("refresh_token":"refresh-token")")
        != std::string::npos);
}

TEST_CASE("OpenAI device flow polls pending responses then exchanges tokens")
{
    int calls       = 0;
    const auto post = [&](const std::string& url,
                          const std::vector<std::string>&, const std::string&,
                          long, std::string& body, long* code) {
        ++calls;
        if (url.ends_with("/deviceauth/token") && calls == 1) {
            *code = 403;
            body  = "pending";
        } else if (url.ends_with("/deviceauth/token")) {
            *code = 200;
            body
                = R"({"authorization_code":"code","code_verifier":"verifier"})";
        } else {
            *code = 200;
            body  = std::string(R"({"access_token":")") + token_with_claims()
                + R"(","id_token":")" + token_with_claims()
                + R"(","refresh_token":"refresh"})";
        }
        return imza::Status::OK;
    };
    imza::OpenAIDeviceCode device;
    device.device_auth_id = "device";
    device.user_code      = "ABCD";
    device.interval       = std::chrono::seconds(1);
    const auto result     = imza::await_openai_device_code(device, { }, post,
        [](std::stop_token, std::chrono::seconds) { return true; });
    CHECK(result.status == imza::Status::OK);
    CHECK(result.credentials.account_id == "account-1");
    CHECK(result.credentials.refresh_token == "refresh");
    CHECK(calls == 3);
}
