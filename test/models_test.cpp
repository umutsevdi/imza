#include <doctest/doctest.h>

#include "loopback_http.h"
#include "network/network.h"

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
