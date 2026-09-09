#include <doctest/doctest.h>
#include <json/json.h>

#include "common/types.h"
#include "network/json_io.h"
#include "network/network.h"
#include "network/sse_parse.h"

namespace {

std::vector<imza::StreamEvent> parse_all(const imza::Provider& p,
    imza::ParseState& state,
    std::initializer_list<std::pair<std::string_view, std::string_view>> blocks)
{
    std::vector<imza::StreamEvent> all;
    for (const auto& [event, data] : blocks) {
        std::vector<imza::StreamEvent> outs;
        p.parse(state, event, data, outs);
        all.insert(all.end(), outs.begin(), outs.end());
    }
    return all;
}

} // namespace

TEST_CASE("OpenAI request shape via factory")
{
    const auto p = imza::get_provider(imza::Route { });

    imza::ChatRequest req;
    req.model = "gpt-4o";
    req.messages.push_back({ imza::Message::Type::SYSTEM, "sys" });
    req.messages.push_back({ imza::Message::Type::USER, "hi" });

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
    imza::Route route;
    route.dialect = imza::ApiStandard::ANTHROPIC;
    const auto p  = imza::get_provider(route);

    imza::ChatRequest req;
    req.model = "claude";
    req.messages.push_back({ imza::Message::Type::SYSTEM, "sys" });
    req.messages.push_back({ imza::Message::Type::USER, "hi" });

    const Json::Value v = p.build(req);
    CHECK(v["model"].asString() == "claude");
    CHECK(v["system"].size() == 1);
    CHECK(v["system"][0].asString() == "sys");
    CHECK(v["messages"].size() == 1);
    CHECK(v["messages"][0]["role"].asString() == "user");
    CHECK(v.isMember("max_tokens"));
    CHECK_FALSE(v.isMember("tools"));
}

TEST_CASE("OpenAI Responses request shape via factory")
{
    imza::Route route;
    route.dialect       = imza::ApiStandard::OPENAI_RESPONSES;
    const auto provider = imza::get_provider(route);

    imza::ToolSpec spec;
    spec.name        = "read";
    spec.description = "read a file";
    spec.parameters  = imza::parse_json(R"({"type":"object"})");

    imza::ChatRequest req;
    req.model             = "gpt-5";
    req.reasoning_effort  = "high";
    req.max_output_tokens = 2048;
    req.tools             = { spec };
    req.messages.push_back({ imza::Message::Type::SYSTEM, "sys" });
    req.messages.push_back({ imza::Message::Type::USER, "hi" });
    imza::Message assistant { imza::Message::Type::ASSISTANT, "checking" };
    assistant.thinking.push_back({ "summary", "encrypted" });
    assistant.tool_calls.push_back({ "call_1", "read", R"({"path":"a"})" });
    req.messages.push_back(std::move(assistant));
    req.messages.push_back(
        { imza::Message::Type::TOOL, "file body", { }, "call_1" });

    const Json::Value value = provider.build(req);
    CHECK(value["model"].asString() == "gpt-5");
    CHECK(value["stream"].asBool());
    CHECK_FALSE(value["store"].asBool());
    CHECK(value["include"][0].asString() == "reasoning.encrypted_content");
    CHECK(value["reasoning"]["effort"].asString() == "high");
    CHECK(value["reasoning"]["summary"].asString() == "auto");
    CHECK(value["max_output_tokens"].asUInt64() == 2048);
    REQUIRE(value["tools"].size() == 1);
    CHECK(value["tools"][0]["name"].asString() == "read");
    CHECK_FALSE(value["tools"][0].isMember("function"));

    REQUIRE(value["input"].size() == 6);
    CHECK(value["input"][0]["role"].asString() == "system");
    CHECK(value["input"][1]["role"].asString() == "user");
    CHECK(value["input"][2]["type"].asString() == "reasoning");
    CHECK(value["input"][2]["encrypted_content"].asString() == "encrypted");
    REQUIRE(value["input"][2]["summary"].size() == 1);
    CHECK(value["input"][2]["summary"][0]["type"].asString() == "summary_text");
    CHECK(value["input"][2]["summary"][0]["text"].asString() == "summary");
    CHECK(value["input"][3]["role"].asString() == "assistant");
    CHECK(value["input"][4]["type"].asString() == "function_call");
    CHECK(value["input"][4]["call_id"].asString() == "call_1");
    CHECK(value["input"][5]["type"].asString() == "function_call_output");
    CHECK(value["input"][5]["output"].asString() == "file body");
}

TEST_CASE("OpenAI Responses reasoning input includes an empty summary array")
{
    imza::Route route;
    route.dialect       = imza::ApiStandard::OPENAI_RESPONSES;
    const auto provider = imza::get_provider(route);

    imza::ChatRequest req;
    req.model = "gpt-5";
    imza::Message assistant { imza::Message::Type::ASSISTANT, "" };
    assistant.thinking.push_back({ "", "encrypted" });
    req.messages.push_back(std::move(assistant));

    const Json::Value value = provider.build(req);
    REQUIRE(value["input"].size() == 2);
    CHECK(value["input"][0]["type"].asString() == "reasoning");
    CHECK(value["input"][0]["summary"].isArray());
    CHECK(value["input"][0]["summary"].empty());
}

TEST_CASE("providers cap requested output tokens")
{
    imza::ChatRequest openai_request;
    openai_request.model             = "gpt-4o";
    openai_request.max_output_tokens = 2048;
    Json::Value openai
        = imza::get_provider(imza::Route { }).build(openai_request);
    CHECK(openai["max_tokens"].asUInt64() == 2048);

    openai_request.reasoning_effort = "low";
    openai = imza::get_provider(imza::Route { }).build(openai_request);
    CHECK_FALSE(openai.isMember("max_tokens"));
    CHECK(openai["max_completion_tokens"].asUInt64() == 2048);

    imza::Route route;
    route.dialect = imza::ApiStandard::ANTHROPIC;
    imza::ChatRequest anthropic_request;
    anthropic_request.model             = "claude";
    anthropic_request.thinking_budget   = 2000;
    anthropic_request.max_output_tokens = 2048;
    const Json::Value anthropic
        = imza::get_provider(route).build(anthropic_request);
    CHECK(anthropic["max_tokens"].asUInt64() == 4048);
}

TEST_CASE("OpenAI serializes tool specs and tool messages")
{
    const auto p = imza::get_provider(imza::Route { });

    imza::ToolSpec spec;
    spec.name        = "read";
    spec.description = "read a file";
    spec.parameters  = imza::parse_json(
        R"({"type":"object","properties":{"path":{"type":"string"}}})");

    imza::ChatRequest req;
    req.model = "gpt-4o";
    req.tools = { spec };
    imza::Message assistant { imza::Message::Type::ASSISTANT, "" };
    assistant.tool_calls.push_back({ "call_1", "read", R"({"path":"a"})" });
    req.messages.push_back(assistant);
    req.messages.push_back(
        { imza::Message::Type::TOOL, "file body", { }, "call_1" });

    const Json::Value v = p.build(req);
    REQUIRE(v["tools"].size() == 1);
    CHECK(v["tools"][0]["type"].asString() == "function");
    CHECK(v["tools"][0]["function"]["name"].asString() == "read");
    CHECK(v["tools"][0]["function"]["description"].asString() == "read a file");
    CHECK(
        v["tools"][0]["function"]["parameters"]["properties"].isMember("path"));
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
    imza::Route route;
    route.dialect = imza::ApiStandard::ANTHROPIC;
    const auto p  = imza::get_provider(route);

    imza::ToolSpec spec;
    spec.name        = "grep";
    spec.description = "search files";
    spec.parameters  = imza::parse_json(
        R"({"type":"object","properties":{"pattern":{"type":"string"}}})");

    imza::ChatRequest req;
    req.model = "claude";
    req.tools = { spec };
    imza::Message assistant { imza::Message::Type::ASSISTANT, "looking" };
    assistant.tool_calls.push_back({ "tu_1", "grep", R"({"pattern":"foo"})" });
    req.messages.push_back(assistant);
    req.messages.push_back(
        { imza::Message::Type::TOOL, "2 matches", { }, "tu_1" });

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
    const auto p = imza::get_provider(imza::Route { });

    imza::ParseState state;
    const auto outs = parse_all(
        p, state, { { "", R"({"choices":[{"delta":{"content":"Hello"}}]})" } });
    REQUIRE(outs.size() == 1);
    CHECK(outs[0].kind == imza::StreamEvent::Kind::CONTENT_DELTA);
    CHECK(outs[0].text == "Hello");
}

TEST_CASE("OpenAI done")
{
    const auto p = imza::get_provider(imza::Route { });

    imza::ParseState state;
    const auto outs = parse_all(p, state, { { "", "[DONE]" } });
    REQUIRE(outs.size() == 1);
    CHECK(outs[0].kind == imza::StreamEvent::Kind::DONE);
    CHECK(state.terminal);
}

TEST_CASE("OpenAI Responses streams text reasoning tools and usage")
{
    imza::Route route;
    route.dialect       = imza::ApiStandard::OPENAI_RESPONSES;
    const auto provider = imza::get_provider(route);

    imza::ParseState state;
    const auto outs = parse_all(provider, state,
        { { "response.reasoning_summary_text.delta",
              R"({"type":"response.reasoning_summary_text.delta","delta":"Thinking"})" },
            { "response.output_text.delta",
                R"({"type":"response.output_text.delta","delta":"Hello"})" },
            { "response.output_item.added",
                R"({"type":"response.output_item.added","output_index":1,"item":{"type":"function_call","call_id":"call_7","name":"read","arguments":""}})" },
            { "response.function_call_arguments.delta",
                R"({"type":"response.function_call_arguments.delta","output_index":1,"delta":"{\"path\":"})" },
            { "response.function_call_arguments.delta",
                R"({"type":"response.function_call_arguments.delta","output_index":1,"delta":"\"a\"}"})" },
            { "response.output_item.done",
                R"({"type":"response.output_item.done","output_index":1,"item":{"type":"function_call","call_id":"call_7","name":"read","arguments":"{\"path\":\"a\"}"}})" },
            { "response.output_item.done",
                R"({"type":"response.output_item.done","output_index":0,"item":{"type":"reasoning","encrypted_content":"secret"}})" },
            { "response.completed",
                R"({"type":"response.completed","response":{"usage":{"input_tokens":100,"output_tokens":20,"total_tokens":120,"input_tokens_details":{"cached_tokens":60,"cache_write_tokens":5}}}})" } });

    REQUIRE(outs.size() == 6);
    CHECK(outs[0].kind == imza::StreamEvent::Kind::REASONING);
    CHECK(outs[0].text == "Thinking");
    CHECK(outs[1].kind == imza::StreamEvent::Kind::CONTENT_DELTA);
    CHECK(outs[1].text == "Hello");
    CHECK(outs[2].kind == imza::StreamEvent::Kind::TOOL_CALL);
    CHECK(outs[2].tool_call.id == "call_7");
    CHECK(outs[2].tool_call.name == "read");
    CHECK(outs[2].tool_call.args == R"({"path":"a"})");
    CHECK(outs[3].kind == imza::StreamEvent::Kind::REASONING);
    CHECK(outs[3].thinking_signature == "secret");
    CHECK(outs[4].kind == imza::StreamEvent::Kind::USAGE);
    CHECK(outs[4].usage.prompt == 100);
    CHECK(outs[4].usage.completion == 20);
    CHECK(outs[4].usage.cached_read == 60);
    CHECK(outs[4].usage.cached_write == 5);
    CHECK(outs[5].kind == imza::StreamEvent::Kind::DONE);
    CHECK(state.terminal);
}

TEST_CASE("OpenAI accumulates fragmented tool calls and flushes")
{
    const auto p = imza::get_provider(imza::Route { });

    imza::ParseState state;
    const auto outs = parse_all(p, state,
        { { "",
              R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_a","type":"function","function":{"name":"read","arguments":"{\"pa"}}]}}]})" },
            { "",
                R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"th\":\"src/main.cpp\"}"}}]}}]})" },
            { "",
                R"({"choices":[{"delta":{"tool_calls":[{"index":1,"id":"call_b","type":"function","function":{"name":"grep","arguments":"{}"}}]}}]})" },
            { "",
                R"({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})" } });

    REQUIRE(outs.size() == 6);
    for (size_t i = 0; i < 3; ++i) {
        CHECK(outs[i].kind == imza::StreamEvent::Kind::CONTENT_DELTA);
    }
    CHECK(outs[3].kind == imza::StreamEvent::Kind::TOOL_CALL);
    CHECK(outs[3].tool_call.id == "call_a");
    CHECK(outs[3].tool_call.name == "read");
    CHECK(outs[3].tool_call.args == R"({"path":"src/main.cpp"})");
    CHECK(outs[4].kind == imza::StreamEvent::Kind::TOOL_CALL);
    CHECK(outs[4].tool_call.id == "call_b");
    CHECK(outs[4].tool_call.name == "grep");
    CHECK(outs[5].kind == imza::StreamEvent::Kind::DONE);

    imza::ParseState drained;
    const auto after = parse_all(p, drained, { { "", "[DONE]" } });
    REQUIRE(after.size() == 1);
    CHECK(after[0].kind == imza::StreamEvent::Kind::DONE);
}

TEST_CASE("Anthropic content delta")
{
    imza::Route route;
    route.dialect = imza::ApiStandard::ANTHROPIC;
    const auto p  = imza::get_provider(route);

    imza::ParseState state;
    const auto outs = parse_all(p, state,
        { { "content_block_delta",
            R"({"type":"content_block_delta","delta":{"type":"text_delta","text":"Hi"}})" } });
    REQUIRE(outs.size() == 1);
    CHECK(outs[0].kind == imza::StreamEvent::Kind::CONTENT_DELTA);
    CHECK(outs[0].text == "Hi");
}

TEST_CASE("Anthropic stop")
{
    imza::Route route;
    route.dialect = imza::ApiStandard::ANTHROPIC;
    const auto p  = imza::get_provider(route);

    imza::ParseState state;
    const auto outs = parse_all(p, state, { { "message_stop", "{}" } });
    REQUIRE(outs.size() == 1);
    CHECK(outs[0].kind == imza::StreamEvent::Kind::DONE);
    CHECK(state.terminal);
}

TEST_CASE("Anthropic assembles tool_use block across deltas")
{
    imza::Route route;
    route.dialect = imza::ApiStandard::ANTHROPIC;
    const auto p  = imza::get_provider(route);

    imza::ParseState state;
    const auto outs = parse_all(p, state,
        { { "content_block_start",
              R"({"type":"content_block_start","index":1,"content_block":{"type":"tool_use","id":"tu_9","name":"write"}})" },
            { "content_block_delta",
                R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"{\"path\":"}})" },
            { "content_block_delta",
                R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"\"b.txt\"}"}})" },
            { "content_block_stop",
                R"({"type":"content_block_stop","index":1})" },
            { "message_stop", R"({"type":"message_stop"})" } });

    REQUIRE(outs.size() == 2);
    CHECK(outs[0].kind == imza::StreamEvent::Kind::TOOL_CALL);
    CHECK(outs[0].tool_call.id == "tu_9");
    CHECK(outs[0].tool_call.name == "write");
    CHECK(outs[0].tool_call.args == R"({"path":"b.txt"})");
    CHECK(outs[1].kind == imza::StreamEvent::Kind::DONE);
}

TEST_CASE("get_provider selects by dialect")
{
    imza::ChatRequest req;
    req.model = "m";

    imza::Route openai;
    CHECK(imza::get_provider(openai)
            .build(req)["stream_options"]["include_usage"]
            .asBool());

    imza::Route anthropic;
    anthropic.dialect = imza::ApiStandard::ANTHROPIC;
    CHECK(imza::get_provider(anthropic).build(req).isMember("max_tokens"));

    imza::Route responses;
    responses.dialect = imza::ApiStandard::OPENAI_RESPONSES;
    CHECK(imza::get_provider(responses).build(req).isMember("input"));
}

TEST_CASE("OpenAI requests include_usage and emits a single usage event")
{
    const auto p = imza::get_provider(imza::Route { });

    const Json::Value built = p.build(imza::ChatRequest { "gpt-4o", { }, { } });
    CHECK(built["stream_options"]["include_usage"].asBool() == true);

    imza::ParseState state;
    const auto outs = parse_all(p, state,
        { { "",
              R"({"choices":[{"delta":{"content":"Hi"},"finish_reason":"stop"}],"usage":{"prompt_tokens":10,"completion_tokens":3,"total_tokens":13}})" },
            { "", "[DONE]" } });
    REQUIRE(outs.size() == 4);
    CHECK(outs[0].kind == imza::StreamEvent::Kind::CONTENT_DELTA);
    CHECK(outs[0].text == "Hi");
    CHECK(outs[1].kind == imza::StreamEvent::Kind::USAGE);
    CHECK(outs[1].usage.prompt == 10);
    CHECK(outs[1].usage.completion == 3);
    CHECK(outs[1].usage.total == 13);
    CHECK(outs[2].kind == imza::StreamEvent::Kind::DONE);
    CHECK(outs[3].kind == imza::StreamEvent::Kind::DONE);
}

TEST_CASE("OpenAI reads usage from choice when top-level absent (Kimi native)")
{
    const auto p = imza::get_provider(imza::Route { });

    imza::ParseState state;
    const auto outs = parse_all(p, state,
        { { "",
            R"({"choices":[{"delta":{},"finish_reason":"stop","usage":{"prompt_tokens":12,"completion_tokens":5,"total_tokens":17}}]})" } });
    REQUIRE(outs.size() == 2);
    CHECK(outs[0].kind == imza::StreamEvent::Kind::USAGE);
    CHECK(outs[0].usage.total == 17);
    CHECK(outs[1].kind == imza::StreamEvent::Kind::DONE);
}

TEST_CASE("OpenAI usage reports cached tokens from prompt_tokens_details")
{
    const auto p = imza::get_provider(imza::Route { });

    imza::ParseState state;
    const auto outs = parse_all(p, state,
        { { "",
              R"({"choices":[{"delta":{}}],"usage":{"prompt_tokens":100,"completion_tokens":4,"total_tokens":104,"prompt_tokens_details":{"cached_tokens":60}}})" },
            { "", "[DONE]" } });
    REQUIRE(outs.size() == 2);
    CHECK(outs[0].kind == imza::StreamEvent::Kind::USAGE);
    CHECK(outs[0].usage.prompt == 100);
    CHECK(outs[0].usage.cached_read == 60);
    CHECK(outs[0].usage.cached_write == 0);
    CHECK(outs[0].usage.completion == 4);
}

TEST_CASE("Anthropic usage folds cache tokens into prompt")
{
    imza::Route route;
    route.dialect = imza::ApiStandard::ANTHROPIC;
    const auto p  = imza::get_provider(route);

    imza::ParseState state;
    const auto outs = parse_all(p, state,
        { { "message_start",
              R"({"type":"message_start","message":{"usage":{"input_tokens":10,"cache_read_input_tokens":60,"cache_creation_input_tokens":30}}})" },
            { "message_delta",
                R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":7}})" },
            { "message_stop", R"({"type":"message_stop"})" } });
    REQUIRE(outs.size() == 2);
    CHECK(outs[0].kind == imza::StreamEvent::Kind::USAGE);
    CHECK(outs[0].usage.cached_read == 60);
    CHECK(outs[0].usage.cached_write == 30);
    CHECK(outs[0].usage.prompt == 100);
    CHECK(outs[0].usage.completion == 7);
    CHECK(outs[0].usage.total == 107);
    CHECK(outs[1].kind == imza::StreamEvent::Kind::DONE);
}

TEST_CASE("Anthropic emits usage from message_start and message_delta")
{
    imza::Route route;
    route.dialect = imza::ApiStandard::ANTHROPIC;
    const auto p  = imza::get_provider(route);

    imza::ParseState state;
    const auto outs = parse_all(p, state,
        { { "message_start",
              R"({"type":"message_start","message":{"usage":{"input_tokens":21,"output_tokens":0}}})" },
            { "content_block_delta",
                R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"ok"}})" },
            { "message_delta",
                R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":7}})" },
            { "message_stop", R"({"type":"message_stop"})" } });
    REQUIRE(outs.size() == 3);
    CHECK(outs[0].kind == imza::StreamEvent::Kind::CONTENT_DELTA);
    CHECK(outs[1].kind == imza::StreamEvent::Kind::USAGE);
    CHECK(outs[1].usage.prompt == 21);
    CHECK(outs[1].usage.completion == 7);
    CHECK(outs[1].usage.total == 28);
    CHECK(outs[2].kind == imza::StreamEvent::Kind::DONE);
}
