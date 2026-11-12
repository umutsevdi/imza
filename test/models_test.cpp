#include <doctest/doctest.h>

#include "common/util.h"
#include "loopback_http.h"
#include "network/network.h"
#include "platform/config.h"
#include "providers/catalog.h"
#include "test_platform.h"

#include <filesystem>
#include <fstream>

namespace {

TEST_CASE("parse_models_response reads and filters successful response shapes")
{
    struct ExpectedModel {
        const char* id;
        const char* name;
        std::optional<std::uint64_t> context_length;
    };
    struct Scenario {
        const char* name;
        const char* body;
        std::vector<ExpectedModel> expected;
        bool check_metadata;
    };
    const std::vector<Scenario> scenarios {
        { "plain OpenAI data",
            R"({"data": [
                {"id": "gpt-4o"},
                {"id": "gpt-4o-mini"}
            ]})",
            { { "gpt-4o", "", std::nullopt },
                { "gpt-4o-mini", "", std::nullopt } },
            true },
        { "rich OpenRouter data sorted by id",
            R"({"data": [
                {"id": "z-ai/glm-5.3", "name": "GLM 5.3", "context_length": 204800},
                {"id": "openai/gpt-5.5", "name": "GPT 5.5"}
            ]})",
            { { "openai/gpt-5.5", "GPT 5.5", std::nullopt },
                { "z-ai/glm-5.3", "GLM 5.3", 204800 } },
            true },
        { "Anthropic-shaped data",
            R"({"data": [
                {"id": "claude-sonnet-5", "display_name": "Claude Sonnet 5"},
                {"id": "claude-haiku-4.5"}
            ]})",
            { { "claude-haiku-4.5", "", std::nullopt },
                { "claude-sonnet-5", "", std::nullopt } },
            false },
        { "deny-listed ids removed",
            R"({"data": [
                {"id": "gpt-4o"},
                {"id": "openai/dall-e-3"},
                {"id": "text-embedding-3-large"},
                {"id": "whisper-large-v3"},
                {"id": "tts-1"},
                {"id": "text-moderation-latest"},
                {"id": "moderation-model-x"},
                {"id": "claude-sonnet-5"}
            ]})",
            { { "claude-sonnet-5", "", std::nullopt },
                { "gpt-4o", "", std::nullopt } },
            false },
        { "entries without ids skipped",
            R"({"data": [
                {"name": "no id here"},
                "a string entry",
                {"id": "real"}
            ]})",
            { { "real", "", std::nullopt } }, false },
    };

    for (const auto& scenario : scenarios) {
        CAPTURE(scenario.name);
        std::vector<imza::ModelInfo> out;
        REQUIRE(imza::parse_models_response(scenario.body, out)
            == imza::Status::OK);
        REQUIRE(out.size() == scenario.expected.size());
        for (std::size_t i = 0; i < out.size(); ++i) {
            CAPTURE(i);
            CHECK(out[i].id == scenario.expected[i].id);
            if (scenario.check_metadata) {
                CHECK(out[i].name == scenario.expected[i].name);
                CHECK(out[i].context_length
                    == scenario.expected[i].context_length);
            }
        }
    }
}

TEST_CASE("parse_models_response rejects malformed bodies")
{
    const std::vector<const char*> bodies { "not json", "[1,2,3]",
        R"({"models": []})", R"({"data": {}})" };

    for (const char* body : bodies) {
        CAPTURE(body);
        std::vector<imza::ModelInfo> out;
        CHECK(
            imza::parse_models_response(body, out) == imza::Status::JSON_ERROR);
    }
}

} // namespace
namespace {

// A route pointing at a loopback server that answers with one scripted
// status, so verify_route exercises the real transport.
// base_url() is only valid after start(), so the caller starts first.
imza::Route loopback_route(
    imza::test::LoopbackHttpServer& server, std::string& seen_body)
{
    server.handle = [&seen_body](imza::test::detail::Socket fd,
                        const imza::test::LoopbackRequest& request) {
        seen_body = request.body;
        if (request.headers.find("Authorization: Bearer good-key")
            != std::string::npos) {
            imza::test::LoopbackHttpServer::respond(fd, "HTTP/1.1 200 OK",
                { "Content-Type: text/event-stream" },
                "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n"
                "data: [DONE]\n\n");
            return;
        }
        imza::test::LoopbackHttpServer::respond(fd, "HTTP/1.1 401 Unauthorized",
            { "Content-Type: application/json" },
            R"({"error":{"message":"invalid api key"}})");
    };

    imza::Route route;
    route.api      = server.base_url();
    route.endpoint = server.base_url() + "/chat/completions";
    route.dialect  = imza::ApiStandard::OPENAI;
    route.auth     = imza::AuthType::BEARER;
    return route;
}

} // namespace

TEST_CASE("verify_route rejects a key the provider does not accept")
{
    imza::test::LoopbackHttpServer server;
    std::string body;
    REQUIRE(server.start());
    imza::Route route = loopback_route(server, body);
    route.api_key     = "not-a-real-key";

    std::string message;
    CHECK(imza::verify_route(route, "some-model", &message)
        == imza::Status::API_ERROR);
    // A completion request, not a model listing.
    CHECK(body.find("\"some-model\"") != std::string::npos);
    CHECK(body.find("\"messages\"") != std::string::npos);
    // The provider's explanation survives, so the dialog can show it.
    CHECK(message == "invalid api key");
}

TEST_CASE("verify_route fails closed without a model or endpoint")
{
    CHECK(
        imza::verify_route(imza::Route { }, "m") == imza::Status::INVALID_URL);
    imza::Route route;
    route.endpoint = "http://127.0.0.1:9/v1/chat/completions";
    CHECK(imza::verify_route(route, "") == imza::Status::CONFIG_ERROR);
}

namespace {

// Writes a fake IRSA web-identity token file and returns its path.
std::string write_token_file(const std::string& token)
{
    const std::filesystem::path dir
        = std::filesystem::temp_directory_path() / "imza-sts-test";
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / "token";
    std::ofstream out(file);
    out << token;
    return imza::portable_string(file);
}

} // namespace

TEST_CASE("resolve_aws_credentials prefers static env keys")
{
    imza::test::set_env("AWS_ACCESS_KEY_ID", "AKIDSTATIC");
    imza::test::set_env("AWS_SECRET_ACCESS_KEY", "static-secret");
    imza::test::unset_env("AWS_WEB_IDENTITY_TOKEN_FILE");
    imza::test::unset_env("AWS_CONTAINER_CREDENTIALS_RELATIVE_URI");
    imza::invalidate_aws_credentials();

    imza::AwsCredentials credentials;
    std::string message;
    REQUIRE(imza::resolve_aws_credentials(credentials, &message)
        == imza::Status::OK);
    CHECK(credentials.access_key == "AKIDSTATIC");
    CHECK(credentials.secret_key == "static-secret");
    imza::invalidate_aws_credentials();
    imza::test::unset_env("AWS_ACCESS_KEY_ID");
    imza::test::unset_env("AWS_SECRET_ACCESS_KEY");
}

TEST_CASE("resolve_aws_credentials fails closed with no ambient source")
{
    imza::test::unset_env("AWS_ACCESS_KEY_ID");
    imza::test::unset_env("AWS_SECRET_ACCESS_KEY");
    imza::test::unset_env("AWS_WEB_IDENTITY_TOKEN_FILE");
    imza::test::unset_env("AWS_CONTAINER_CREDENTIALS_RELATIVE_URI");
    imza::invalidate_aws_credentials();

    imza::AwsCredentials credentials;
    std::string message;
    CHECK(imza::resolve_aws_credentials(credentials, &message)
        == imza::Status::CONFIG_ERROR);
    CHECK(!message.empty());
}

TEST_CASE("IRSA exchange resolves credentials and caches until expiry")
{
    imza::test::allow_loopback_direct();
    imza::test::LoopbackHttpServer sts;
    REQUIRE(sts.start());
    int exchanges = 0;
    std::string seen_token;
    sts.handle = [&](imza::test::detail::Socket fd,
                     const imza::test::LoopbackRequest& request) {
        exchanges += 1;
        CHECK(request.body.find("Action=AssumeRoleWithWebIdentity")
            != std::string::npos);
        CHECK(request.body.find("RoleArn=arn%3Aaws%3Aiam%3A%3A1234")
            != std::string::npos);
        const auto token_at = request.body.find("WebIdentityToken=");
        REQUIRE(token_at != std::string::npos);
        seen_token = request.body.substr(token_at + 17);
        const std::string body
            = R"({"AssumeRoleWithWebIdentityResponse":{"AssumeRoleWithWebIdentityResult":{"Credentials":{"AccessKeyId":"AKIDSTS","SecretAccessKey":"sts-secret","SessionToken":"sts-token","Expiration":"2100-01-01T00:00:00Z"}}}})";
        imza::test::LoopbackHttpServer::respond(
            fd, "HTTP/1.1 200 OK", { "Content-Type: application/json" }, body);
    };

    const std::string token_file = write_token_file("fake-jwt-token");
    imza::test::set_env("AWS_WEB_IDENTITY_TOKEN_FILE", token_file);
    imza::test::set_env("AWS_ROLE_ARN", "arn:aws:iam::1234:role/agents");
    imza::test::unset_env("AWS_ACCESS_KEY_ID");
    imza::test::unset_env("AWS_SECRET_ACCESS_KEY");
    imza::test::set_env("IMZA_TEST_STS_URL", sts.base_url() + "/");
    imza::invalidate_aws_credentials();

    imza::AwsCredentials credentials;
    std::string message;
    REQUIRE(imza::resolve_aws_credentials(credentials, &message)
        == imza::Status::OK);
    CHECK(credentials.access_key == "AKIDSTS");
    CHECK(credentials.secret_key == "sts-secret");
    CHECK(credentials.session_token == "sts-token");
    CHECK(seen_token == "fake-jwt-token");
    CHECK(exchanges == 1);

    // Second resolution within the (far future) expiry reuses the cache.
    imza::AwsCredentials again;
    REQUIRE(imza::resolve_aws_credentials(again, &message) == imza::Status::OK);
    CHECK(again.access_key == "AKIDSTS");
    CHECK(exchanges == 1);

    imza::invalidate_aws_credentials();
    imza::test::unset_env("IMZA_TEST_STS_URL");
    imza::test::unset_env("AWS_WEB_IDENTITY_TOKEN_FILE");
    imza::test::unset_env("AWS_ROLE_ARN");
    std::filesystem::remove_all(
        std::filesystem::path(token_file).parent_path());
}

TEST_CASE("expired STS credentials trigger a re-exchange")
{
    imza::test::allow_loopback_direct();
    imza::test::LoopbackHttpServer sts;
    REQUIRE(sts.start());
    int exchanges = 0;
    sts.handle    = [&](imza::test::detail::Socket fd,
                        const imza::test::LoopbackRequest&) {
        exchanges += 1;
        // Expiration in the past: resolve_aws_credentials must treat the
        // cache as stale and exchange again.
        const std::string body
            = R"({"AssumeRoleWithWebIdentityResponse":{"AssumeRoleWithWebIdentityResult":{"Credentials":{"AccessKeyId":"AKIDOLD","SecretAccessKey":"s","SessionToken":"t","Expiration":"1970-01-01T00:01:00Z"}}}})";
        imza::test::LoopbackHttpServer::respond(
            fd, "HTTP/1.1 200 OK", { "Content-Type: application/json" }, body);
    };

    const std::string token_file = write_token_file("jwt");
    imza::test::set_env("AWS_WEB_IDENTITY_TOKEN_FILE", token_file);
    imza::test::set_env("AWS_ROLE_ARN", "arn:aws:iam::1234:role/agents");
    imza::test::unset_env("AWS_ACCESS_KEY_ID");
    imza::test::unset_env("AWS_SECRET_ACCESS_KEY");
    imza::test::set_env("IMZA_TEST_STS_URL", sts.base_url() + "/");
    imza::invalidate_aws_credentials();

    imza::AwsCredentials credentials;
    std::string message;
    REQUIRE(imza::resolve_aws_credentials(credentials, &message)
        == imza::Status::OK);
    CHECK(credentials.access_key == "AKIDOLD");

    imza::AwsCredentials again;
    REQUIRE(imza::resolve_aws_credentials(again, &message) == imza::Status::OK);
    CHECK(exchanges == 2);

    imza::invalidate_aws_credentials();
    imza::test::unset_env("IMZA_TEST_STS_URL");
    imza::test::unset_env("AWS_WEB_IDENTITY_TOKEN_FILE");
    imza::test::unset_env("AWS_ROLE_ARN");
    std::filesystem::remove_all(
        std::filesystem::path(token_file).parent_path());
}

TEST_CASE("keyless bedrock routes carry AWS_SIGV4; keyed ones stay bearer")
{
    imza::Catalog catalog;
    imza::CachedProvider bedrock;
    bedrock.api
        = "https://bedrock-runtime.${AWS_REGION}.amazonaws.com/openai/v1";
    catalog.providers["amazon-bedrock"] = bedrock;

    imza::Connection keyless;
    keyless.id = "amazon-bedrock";
    const imza::Route keyless_route
        = imza::resolve_route(keyless, catalog, imza::ApiStandard::OPENAI);
    CHECK(keyless_route.auth == imza::AuthType::AWS_SIGV4);

    imza::Connection keyed;
    keyed.id      = "amazon-bedrock";
    keyed.api_key = "bedrock-key";
    const imza::Route keyed_route
        = imza::resolve_route(keyed, catalog, imza::ApiStandard::OPENAI);
    CHECK(keyed_route.auth == imza::AuthType::BEARER);
}
