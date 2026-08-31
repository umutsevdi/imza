#include <doctest/doctest.h>
#include <json/json.h>

#include "network.h"
#include "types.h"

namespace {

std::vector<ursa::StreamEvent> parse_all(
    const ursa::Provider& p, ursa::ParseState& state,
    std::initializer_list<std::pair<std::string_view, std::string_view>> blocks)
{
    std::vector<ursa::StreamEvent> all;
    for (const auto& [event, data] : blocks) {
        std::vector<ursa::StreamEvent> outs;
        p.parse(state, event, data, outs);
        all.insert(all.end(), outs.begin(), outs.end());
    }
    return all;
}

} // namespace

TEST_CASE("OpenAI request shape via factory")
{
    const auto p = ursa::get_provider(ursa::Route { });

    ursa::ChatRequest req;
    req.model = "gpt-4o";
    req.messages.push_back({ ursa::Message::Type::SYSTEM, "sys" });
    req.messages.push_back({ ursa::Message::Type::USER, "hi" });

    const Json::Value v = p.build(req);
    CHECK(v["model"].asString() == "gpt-4o");
    CHECK(v["stream"].asBool() == true);
    CHECK(v["messages"].size() == 2);
    CHECK(v["messages"][0]["role"].asString() == "system");
    CHECK(v["messages"][1]["role"].asString() == "user");
    CHECK(v["messages"][1]["content"].asString() == "hi");
    CHECK_FALSE(v.isMember("tools"));
}

TEST_CASE("Anthropic request shape via factory")
{
    ursa::Route route;
    route.dialect = ursa::ApiStandard::ANTHROPIC;
    const auto p = ursa::get_provider(route);

    ursa::ChatRequest req;
    req.model = "claude";
    req.messages.push_back({ ursa::Message::Type::SYSTEM, "sys" });
    req.messages.push_back({ ursa::Message::Type::USER, "hi" });

    const Json::Value v = p.build(req);
    CHECK(v["model"].asString() == "claude");
    CHECK(v["system"].size() == 1);
    CHECK(v["system"][0].asString() == "sys");
    CHECK(v["messages"].size() == 1);
    CHECK(v["messages"][0]["role"].asString() == "user");
    CHECK(v.isMember("max_tokens"));
    CHECK_FALSE(v.isMember("tools"));
}

TEST_CASE("providers cap requested output tokens")
{
    ursa::ChatRequest openai_request;
    openai_request.model             = "gpt-4o";
    openai_request.max_output_tokens = 2048;
    Json::Value openai
        = ursa::get_provider(ursa::Route { }).build(openai_request);
    CHECK(openai["max_tokens"].asUInt64() == 2048);

    openai_request.reasoning_effort = "low";
    openai = ursa::get_provider(ursa::Route { }).build(openai_request);
    CHECK_FALSE(openai.isMember("max_tokens"));
    CHECK(openai["max_completion_tokens"].asUInt64() == 2048);

    ursa::Route route;
    route.dialect = ursa::ApiStandard::ANTHROPIC;
    ursa::ChatRequest anthropic_request;
    anthropic_request.model             = "claude";
    anthropic_request.thinking_budget   = 2000;
    anthropic_request.max_output_tokens = 2048;
    const Json::Value anthropic
        = ursa::get_provider(route).build(anthropic_request);
    CHECK(anthropic["max_tokens"].asUInt64() == 4048);
}

TEST_CASE("OpenAI serializes tool specs and tool messages")
{
    const auto p = ursa::get_provider(ursa::Route { });

    ursa::ToolSpec spec;
    spec.name        = "read";
    spec.description = "read a file";
    spec.parameters   = ursa::parse_json(
        R"({"type":"object","properties":{"path":{"type":"string"}}})");

    ursa::ChatRequest req;
    req.model     = "gpt-4o";
    req.tools     = { spec };
    ursa::Message assistant { ursa::Message::Type::ASSISTANT, "" };
    assistant.tool_calls.push_back({ "call_1", "read", R"({"path":"a"})" });
    req.messages.push_back(assistant);
    req.messages.push_back(
        { ursa::Message::Type::TOOL, "file body", { }, "call_1" });

    const Json::Value v = p.build(req);
    REQUIRE(v["tools"].size() == 1);
    CHECK(v["tools"][0]["type"].asString() == "function");
    CHECK(v["tools"][0]["function"]["name"].asString() == "read");
    CHECK(v["tools"][0]["function"]["description"].asString() == "read a file");
    CHECK(v["tools"][0]["function"]["parameters"]["properties"].isMember(
        "path"));
    CHECK(v["tool_choice"].asString() == "auto");

    const Json::Value& asst = v["messages"][0];
    CHECK(asst["role"].asString() == "assistant");
    REQUIRE(asst["tool_calls"].size() == 1);
    CHECK(asst["tool_calls"][0]["id"].asString() == "call_1");
    CHECK(asst["tool_calls"][0]["function"]["name"].asString() == "read");
    CHECK(asst["tool_calls"][0]["function"]["arguments"].asString()
        == R"({"path":"a"})");

    const Json::Value& tool = v["messages"][1];
    CHECK(tool["role"].asString() == "tool");
    CHECK(tool["content"].asString() == "file body");
    CHECK(tool["tool_call_id"].asString() == "call_1");
}

TEST_CASE("Anthropic serializes tool specs and tool_result blocks")
{
    ursa::Route route;
    route.dialect = ursa::ApiStandard::ANTHROPIC;
    const auto p = ursa::get_provider(route);

    ursa::ToolSpec spec;
    spec.name        = "grep";
    spec.description = "search files";
    spec.parameters   = ursa::parse_json(
        R"({"type":"object","properties":{"pattern":{"type":"string"}}})");

    ursa::ChatRequest req;
    req.model     = "claude";
    req.tools     = { spec };
    ursa::Message assistant { ursa::Message::Type::ASSISTANT, "looking" };
    assistant.tool_calls.push_back({ "tu_1", "grep", R"({"pattern":"foo"})" });
    req.messages.push_back(assistant);
    req.messages.push_back(
        { ursa::Message::Type::TOOL, "2 matches", { }, "tu_1" });

    const Json::Value v = p.build(req);
    REQUIRE(v["tools"].size() == 1);
    CHECK(v["tools"][0]["name"].asString() == "grep");
    CHECK(v["tools"][0]["input_schema"]["properties"].isMember("pattern"));

    const Json::Value& asst = v["messages"][0];
    REQUIRE(asst["content"].isArray());
    CHECK(asst["content"][0]["type"].asString() == "text");
    CHECK(asst["content"][0]["text"].asString() == "looking");
    CHECK(asst["content"][1]["type"].asString() == "tool_use");
    CHECK(asst["content"][1]["id"].asString() == "tu_1");
    CHECK(asst["content"][1]["input"]["pattern"].asString() == "foo");

    const Json::Value& result = v["messages"][1];
    CHECK(result["role"].asString() == "user");
    REQUIRE(result["content"].isArray());
    CHECK(result["content"][0]["type"].asString() == "tool_result");
    CHECK(result["content"][0]["tool_use_id"].asString() == "tu_1");
    CHECK(result["content"][0]["content"].asString() == "2 matches");
}

TEST_CASE("OpenAI content delta")
{
    const auto p = ursa::get_provider(ursa::Route { });

    ursa::ParseState state;
    const auto outs = parse_all(p, state,
        { { "", R"({"choices":[{"delta":{"content":"Hello"}}]})" } });
    REQUIRE(outs.size() == 1);
    CHECK(outs[0].kind == ursa::StreamEvent::Kind::CONTENT_DELTA);
    CHECK(outs[0].text == "Hello");
}

TEST_CASE("OpenAI done")
{
    const auto p = ursa::get_provider(ursa::Route { });

    ursa::ParseState state;
    const auto outs = parse_all(p, state, { { "", "[DONE]" } });
    REQUIRE(outs.size() == 1);
    CHECK(outs[0].kind == ursa::StreamEvent::Kind::DONE);
    CHECK(state.terminal);
}

TEST_CASE("OpenAI accumulates fragmented tool calls and flushes")
{
    const auto p = ursa::get_provider(ursa::Route { });

    ursa::ParseState state;
    const auto outs = parse_all(p, state,
        { { "",
            R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_a","type":"function","function":{"name":"read","arguments":"{\"pa"}}]}}]})" },
            { "",
            R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"th\":\"src/main.cpp\"}"}}]}}]})" },
            { "",
            R"({"choices":[{"delta":{"tool_calls":[{"index":1,"id":"call_b","type":"function","function":{"name":"grep","arguments":"{}"}}]}}]})" },
            { "", R"({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})" } });

    REQUIRE(outs.size() == 6);
    for (size_t i = 0; i < 3; ++i) {
        CHECK(outs[i].kind == ursa::StreamEvent::Kind::CONTENT_DELTA);
    }
    CHECK(outs[3].kind == ursa::StreamEvent::Kind::TOOL_CALL);
    CHECK(outs[3].tool_call.id == "call_a");
    CHECK(outs[3].tool_call.name == "read");
    CHECK(outs[3].tool_call.args == R"({"path":"src/main.cpp"})");
    CHECK(outs[4].kind == ursa::StreamEvent::Kind::TOOL_CALL);
    CHECK(outs[4].tool_call.id == "call_b");
    CHECK(outs[4].tool_call.name == "grep");
    CHECK(outs[5].kind == ursa::StreamEvent::Kind::DONE);

    ursa::ParseState drained;
    const auto after = parse_all(p, drained, { { "", "[DONE]" } });
    REQUIRE(after.size() == 1);
    CHECK(after[0].kind == ursa::StreamEvent::Kind::DONE);
}

TEST_CASE("Anthropic content delta")
{
    ursa::Route route;
    route.dialect = ursa::ApiStandard::ANTHROPIC;
    const auto p = ursa::get_provider(route);

    ursa::ParseState state;
    const auto outs
        = parse_all(p, state,
            { { "content_block_delta",
                R"({"type":"content_block_delta","delta":{"type":"text_delta","text":"Hi"}})" } });
    REQUIRE(outs.size() == 1);
    CHECK(outs[0].kind == ursa::StreamEvent::Kind::CONTENT_DELTA);
    CHECK(outs[0].text == "Hi");
}

TEST_CASE("Anthropic stop")
{
    ursa::Route route;
    route.dialect = ursa::ApiStandard::ANTHROPIC;
    const auto p = ursa::get_provider(route);

    ursa::ParseState state;
    const auto outs = parse_all(p, state, { { "message_stop", "{}" } });
    REQUIRE(outs.size() == 1);
    CHECK(outs[0].kind == ursa::StreamEvent::Kind::DONE);
    CHECK(state.terminal);
}

TEST_CASE("Anthropic assembles tool_use block across deltas")
{
    ursa::Route route;
    route.dialect = ursa::ApiStandard::ANTHROPIC;
    const auto p = ursa::get_provider(route);

    ursa::ParseState state;
    const auto outs = parse_all(p, state,
        { { "content_block_start",
            R"({"type":"content_block_start","index":1,"content_block":{"type":"tool_use","id":"tu_9","name":"write"}})" },
            { "content_block_delta",
            R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"{\"path\":"}})" },
            { "content_block_delta",
            R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"\"b.txt\"}"}})" },
            { "content_block_stop", R"({"type":"content_block_stop","index":1})" },
            { "message_stop",
                R"({"type":"message_stop"})" } });

    REQUIRE(outs.size() == 2);
    CHECK(outs[0].kind == ursa::StreamEvent::Kind::TOOL_CALL);
    CHECK(outs[0].tool_call.id == "tu_9");
    CHECK(outs[0].tool_call.name == "write");
    CHECK(outs[0].tool_call.args == R"({"path":"b.txt"})");
    CHECK(outs[1].kind == ursa::StreamEvent::Kind::DONE);
}

TEST_CASE("get_provider selects by dialect")
{
    ursa::ChatRequest req;
    req.model = "m";

    ursa::Route openai;
    CHECK(ursa::get_provider(openai)
              .build(req)["stream_options"]["include_usage"]
              .asBool());

    ursa::Route anthropic;
    anthropic.dialect = ursa::ApiStandard::ANTHROPIC;
    CHECK(ursa::get_provider(anthropic).build(req).isMember("max_tokens"));
}

TEST_CASE("OpenAI requests include_usage and emits a single usage event")
{
    const auto p = ursa::get_provider(ursa::Route { });

    const Json::Value built = p.build(ursa::ChatRequest { "gpt-4o", { }, { } });
    CHECK(built["stream_options"]["include_usage"].asBool() == true);

    ursa::ParseState state;
    const auto outs = parse_all(p, state,
        { { "",
              R"({"choices":[{"delta":{"content":"Hi"},"finish_reason":"stop"}],"usage":{"prompt_tokens":10,"completion_tokens":3,"total_tokens":13}})" },
          { "", "[DONE]" } });
    REQUIRE(outs.size() == 4);
    CHECK(outs[0].kind == ursa::StreamEvent::Kind::CONTENT_DELTA);
    CHECK(outs[0].text == "Hi");
    CHECK(outs[1].kind == ursa::StreamEvent::Kind::USAGE);
    CHECK(outs[1].usage.prompt == 10);
    CHECK(outs[1].usage.completion == 3);
    CHECK(outs[1].usage.total == 13);
    CHECK(outs[2].kind == ursa::StreamEvent::Kind::DONE);
    CHECK(outs[3].kind == ursa::StreamEvent::Kind::DONE);
}

TEST_CASE("OpenAI reads usage from choice when top-level absent (Kimi native)")
{
    const auto p = ursa::get_provider(ursa::Route { });

    ursa::ParseState state;
    const auto outs = parse_all(p, state,
        { { "",
              R"({"choices":[{"delta":{},"finish_reason":"stop","usage":{"prompt_tokens":12,"completion_tokens":5,"total_tokens":17}}]})" } });
    REQUIRE(outs.size() == 2);
    CHECK(outs[0].kind == ursa::StreamEvent::Kind::USAGE);
    CHECK(outs[0].usage.total == 17);
    CHECK(outs[1].kind == ursa::StreamEvent::Kind::DONE);
}

TEST_CASE("OpenAI usage reports cached tokens from prompt_tokens_details")
{
    const auto p = ursa::get_provider(ursa::Route { });

    ursa::ParseState state;
    const auto outs = parse_all(p, state,
        { { "",
              R"({"choices":[{"delta":{}}],"usage":{"prompt_tokens":100,"completion_tokens":4,"total_tokens":104,"prompt_tokens_details":{"cached_tokens":60}}})" },
            { "", "[DONE]" } });
    REQUIRE(outs.size() == 2);
    CHECK(outs[0].kind == ursa::StreamEvent::Kind::USAGE);
    CHECK(outs[0].usage.prompt == 100);
    CHECK(outs[0].usage.cached_read == 60);
    CHECK(outs[0].usage.cached_write == 0);
    CHECK(outs[0].usage.completion == 4);
}

TEST_CASE("Anthropic usage folds cache tokens into prompt")
{
    ursa::Route route;
    route.dialect = ursa::ApiStandard::ANTHROPIC;
    const auto p = ursa::get_provider(route);

    ursa::ParseState state;
    const auto outs = parse_all(p, state,
        { { "message_start",
              R"({"type":"message_start","message":{"usage":{"input_tokens":10,"cache_read_input_tokens":60,"cache_creation_input_tokens":30}}})" },
          { "message_delta",
              R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":7}})" },
          { "message_stop", R"({"type":"message_stop"})" } });
    REQUIRE(outs.size() == 2);
    CHECK(outs[0].kind == ursa::StreamEvent::Kind::USAGE);
    CHECK(outs[0].usage.cached_read == 60);
    CHECK(outs[0].usage.cached_write == 30);
    CHECK(outs[0].usage.prompt == 100);
    CHECK(outs[0].usage.completion == 7);
    CHECK(outs[0].usage.total == 107);
    CHECK(outs[1].kind == ursa::StreamEvent::Kind::DONE);
}

TEST_CASE("Anthropic emits usage from message_start and message_delta")
{
    ursa::Route route;
    route.dialect = ursa::ApiStandard::ANTHROPIC;
    const auto p = ursa::get_provider(route);

    ursa::ParseState state;
    const auto outs = parse_all(p, state,
        { { "message_start",
              R"({"type":"message_start","message":{"usage":{"input_tokens":21,"output_tokens":0}}})" },
          { "content_block_delta",
              R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"ok"}})" },
          { "message_delta",
              R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":7}})" },
          { "message_stop", R"({"type":"message_stop"})" } });
    REQUIRE(outs.size() == 3);
    CHECK(outs[0].kind == ursa::StreamEvent::Kind::CONTENT_DELTA);
    CHECK(outs[1].kind == ursa::StreamEvent::Kind::USAGE);
    CHECK(outs[1].usage.prompt == 21);
    CHECK(outs[1].usage.completion == 7);
    CHECK(outs[1].usage.total == 28);
    CHECK(outs[2].kind == ursa::StreamEvent::Kind::DONE);
}
