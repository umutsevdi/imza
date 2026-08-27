#include <doctest/doctest.h>

#include "network.h"

namespace {

TEST_CASE("parse_models_response reads plain OpenAI fixture")
{
    const std::string body = R"({"data": [
        {"id": "gpt-4o"},
        {"id": "gpt-4o-mini"}
    ]})";
    std::vector<ursa::ModelInfo> out;
    REQUIRE(ursa::parse_models_response(body, out) == ursa::Status::OK);
    REQUIRE(out.size() == 2);
    CHECK(out[0].id == "gpt-4o");
    CHECK(out[0].name.empty());
    CHECK_FALSE(out[0].context_length.has_value());
}

TEST_CASE("parse_models_response reads rich OpenRouter fixture")
{
    const std::string body = R"({"data": [
        {"id": "z-ai/glm-5.3", "name": "GLM 5.3", "context_length": 204800},
        {"id": "openai/gpt-5.5", "name": "GPT 5.5"}
    ]})";
    std::vector<ursa::ModelInfo> out;
    REQUIRE(ursa::parse_models_response(body, out) == ursa::Status::OK);
    REQUIRE(out.size() == 2);
    CHECK(out[0].id == "openai/gpt-5.5");
    CHECK(out[0].name == "GPT 5.5");
    CHECK_FALSE(out[0].context_length.has_value());
    CHECK(out[1].id == "z-ai/glm-5.3");
    CHECK(out[1].name == "GLM 5.3");
    REQUIRE(out[1].context_length.has_value());
    CHECK(*out[1].context_length == 204800);
}

TEST_CASE("parse_models_response reads Anthropic-shaped fixture")
{
    const std::string body = R"({"data": [
        {"id": "claude-sonnet-5", "display_name": "Claude Sonnet 5"},
        {"id": "claude-haiku-4.5"}
    ]})";
    std::vector<ursa::ModelInfo> out;
    REQUIRE(ursa::parse_models_response(body, out) == ursa::Status::OK);
    REQUIRE(out.size() == 2);
    CHECK(out[0].id == "claude-haiku-4.5");
    CHECK(out[1].id == "claude-sonnet-5");
}

TEST_CASE("parse_models_response sorts by id")
{
    const std::string body = R"({"data": [
        {"id": "c-model"}, {"id": "a-model"}, {"id": "b-model"}
    ]})";
    std::vector<ursa::ModelInfo> out;
    REQUIRE(ursa::parse_models_response(body, out) == ursa::Status::OK);
    REQUIRE(out.size() == 3);
    CHECK(out[0].id == "a-model");
    CHECK(out[1].id == "b-model");
    CHECK(out[2].id == "c-model");
}

TEST_CASE("parse_models_response drops deny-listed ids")
{
    const std::string body = R"({"data": [
        {"id": "gpt-4o"},
        {"id": "openai/dall-e-3"},
        {"id": "text-embedding-3-large"},
        {"id": "whisper-large-v3"},
        {"id": "tts-1"},
        {"id": "text-moderation-latest"},
        {"id": "moderation-model-x"},
        {"id": "claude-sonnet-5"}
    ]})";
    std::vector<ursa::ModelInfo> out;
    REQUIRE(ursa::parse_models_response(body, out) == ursa::Status::OK);
    REQUIRE(out.size() == 2);
    CHECK(out[0].id == "claude-sonnet-5");
    CHECK(out[1].id == "gpt-4o");
}

TEST_CASE("parse_models_response rejects malformed bodies")
{
    std::vector<ursa::ModelInfo> out;
    CHECK(ursa::parse_models_response("not json", out)
        == ursa::Status::JSON_ERROR);
    CHECK(ursa::parse_models_response("[1,2,3]", out)
        == ursa::Status::JSON_ERROR);
    CHECK(ursa::parse_models_response(R"({"models": []})", out)
        == ursa::Status::JSON_ERROR);
    CHECK(ursa::parse_models_response(R"({"data": {}})", out)
        == ursa::Status::JSON_ERROR);
}

TEST_CASE("parse_models_response skips entries without id")
{
    const std::string body = R"({"data": [
        {"name": "no id here"},
        "a string entry",
        {"id": "real"}
    ]})";
    std::vector<ursa::ModelInfo> out;
    REQUIRE(ursa::parse_models_response(body, out) == ursa::Status::OK);
    REQUIRE(out.size() == 1);
    CHECK(out[0].id == "real");
}

} // namespace
