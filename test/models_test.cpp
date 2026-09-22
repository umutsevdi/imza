#include <doctest/doctest.h>

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
