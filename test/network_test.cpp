#include <doctest/doctest.h>

#include "common/types.h"
#include "common/util.h"
#include "network/json.h"
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

// Accessor helpers over the dynamic JSON value: pointer chase without
// json_t::operator[]'s missing-key throw.
const imza::JsonValue* at(const imza::JsonValue& v, std::string_view key)
{
    return imza::find_member(v, key);
}

const imza::JsonValue& idx(const imza::JsonValue& v, std::size_t i)
{
    return v.get<imza::JsonValue::array_t>().at(i);
}

// Dialect build() returns the serialized body; parse it for DOM checks.
const imza::JsonValue wire(const std::string& body)
{
    return imza::parse_json(body);
}

std::string str(const imza::JsonValue* v)
{
    return v != nullptr && v->is_string() ? v->as<std::string>() : "";
}

std::string str(const imza::JsonValue& v)
{
    return v.is_string() ? v.as<std::string>() : "";
}

double num(const imza::JsonValue& v)
{
    return v.is_number() ? v.as<double>() : -1;
}

bool truthy(const imza::JsonValue& v)
{
    return v.is_boolean() && v.get<bool>();
}

double num(const imza::JsonValue* v)
{
    return v != nullptr && v->is_number() ? v->as<double>() : -1;
}

bool truthy(const imza::JsonValue* v)
{
    return v != nullptr && v->is_boolean() && v->get<bool>();
}

bool has(const imza::JsonValue& v, std::string_view key)
{
    return at(v, key) != nullptr;
}

std::size_t len(const imza::JsonValue* v)
{
    if (v == nullptr) {
        return 0;
    }
    if (v->is_array()) {
        return v->get<imza::JsonValue::array_t>().size();
    }
    if (v->is_object()) {
        return v->get<imza::JsonValue::object_t>().size();
    }
    return 0;
}

struct JsonTestPayload {
    std::string name;
    int count = 0;
    std::optional<int> note;
};

} // namespace

TEST_CASE("OpenAI request shape via factory")
{
    const auto p = imza::get_provider(imza::Route { });

    imza::ChatRequest req;
    req.model = "gpt-4o";
    req.messages.push_back({ imza::Message::Type::SYSTEM, "sys" });
    req.messages.push_back({ imza::Message::Type::USER, "hi" });

    const imza::JsonValue v = wire(p.build(req));
    CHECK(str(at(v, "model")) == "gpt-4o");
    CHECK(truthy(at(v, "stream")));
    CHECK(len(at(v, "messages")) == 2);
    CHECK(str(at(idx(*at(v, "messages"), 0), "role")) == "system");
    CHECK(str(at(idx(*at(v, "messages"), 1), "role")) == "user");
    CHECK(str(at(idx(*at(v, "messages"), 1), "content")) == "hi");
    CHECK_FALSE(has(v, "tools"));
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

    const imza::JsonValue v = wire(p.build(req));
    CHECK(str(at(v, "model")) == "claude");
    CHECK(len(at(v, "system")) == 1);
    CHECK(str(idx(*at(v, "system"), 0)) == "sys");
    CHECK(len(at(v, "messages")) == 1);
    CHECK(str(at(idx(*at(v, "messages"), 0), "role")) == "user");
    CHECK(has(v, "max_tokens"));
    CHECK_FALSE(has(v, "tools"));
}

TEST_CASE("OpenAI Responses request shape via factory")
{
    imza::Route route;
    route.dialect       = imza::ApiStandard::OPENAI_RESPONSES;
    const auto provider = imza::get_provider(route);

    imza::ToolSpec spec;
    spec.name        = "read";
    spec.description = "read a file";
    spec.parameters  = R"({"type":"object"})";

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

    const imza::JsonValue value = wire(provider.build(req));
    CHECK(str(at(value, "model")) == "gpt-5");
    CHECK(truthy(at(value, "stream")));
    const imza::JsonValue* store = at(value, "store");
    REQUIRE(store != nullptr);
    REQUIRE(store->is_boolean());
    CHECK_FALSE(store->get<bool>());
    CHECK(str(idx(*at(value, "include"), 0)) == "reasoning.encrypted_content");
    CHECK(str(at(*at(value, "reasoning"), "effort")) == "high");
    CHECK(str(at(*at(value, "reasoning"), "summary")) == "auto");
    CHECK(num(at(value, "max_output_tokens")) == 2048);
    REQUIRE(len(at(value, "tools")) == 1);
    CHECK(str(at(idx(*at(value, "tools"), 0), "name")) == "read");
    CHECK_FALSE(has(idx(*at(value, "tools"), 0), "function"));

    REQUIRE(len(at(value, "input")) == 6);
    const auto input
        = [&value](std::size_t i) { return idx(*at(value, "input"), i); };
    CHECK(str(at(input(0), "role")) == "system");
    CHECK(str(at(input(1), "role")) == "user");
    CHECK(str(at(input(2), "type")) == "reasoning");
    CHECK(str(at(input(2), "encrypted_content")) == "encrypted");
    REQUIRE(len(at(input(2), "summary")) == 1);
    CHECK(str(at(idx(*at(input(2), "summary"), 0), "type")) == "summary_text");
    CHECK(str(at(idx(*at(input(2), "summary"), 0), "text")) == "summary");
    CHECK(str(at(input(3), "role")) == "assistant");
    CHECK(str(at(input(4), "type")) == "function_call");
    CHECK(str(at(input(4), "call_id")) == "call_1");
    CHECK(str(at(input(5), "type")) == "function_call_output");
    CHECK(str(at(input(5), "output")) == "file body");
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

    const imza::JsonValue value = wire(provider.build(req));
    REQUIRE(len(at(value, "input")) == 2);
    const imza::JsonValue& first = idx(*at(value, "input"), 0);
    CHECK(str(at(first, "type")) == "reasoning");
    const imza::JsonValue* summary = at(first, "summary");
    REQUIRE(summary != nullptr);
    REQUIRE(summary->is_array());
    CHECK(summary->get<imza::JsonValue::array_t>().empty());
}

TEST_CASE("providers cap requested output tokens")
{
    imza::ChatRequest openai_request;
    openai_request.model             = "gpt-4o";
    openai_request.max_output_tokens = 2048;
    imza::JsonValue openai
        = wire(imza::get_provider(imza::Route { }).build(openai_request));
    CHECK(num(at(openai, "max_tokens")) == 2048);

    openai_request.reasoning_effort = "low";
    openai = wire(imza::get_provider(imza::Route { }).build(openai_request));
    CHECK_FALSE(has(openai, "max_tokens"));
    CHECK(num(at(openai, "max_completion_tokens")) == 2048);

    imza::Route route;
    route.dialect = imza::ApiStandard::ANTHROPIC;
    imza::ChatRequest anthropic_request;
    anthropic_request.model             = "claude";
    anthropic_request.thinking_budget   = 2000;
    anthropic_request.max_output_tokens = 2048;
    const imza::JsonValue anthropic
        = wire(imza::get_provider(route).build(anthropic_request));
    CHECK(num(at(anthropic, "max_tokens")) == 4048);
}

TEST_CASE("OpenAI serializes tool specs and tool messages")
{
    const auto p = imza::get_provider(imza::Route { });

    imza::ToolSpec spec;
    spec.name        = "read";
    spec.description = "read a file";
    spec.parameters
        = R"({"type":"object","properties":{"path":{"type":"string"}}})";

    imza::ChatRequest req;
    req.model = "gpt-4o";
    req.tools = { spec };
    imza::Message assistant { imza::Message::Type::ASSISTANT, "" };
    assistant.tool_calls.push_back({ "call_1", "read", R"({"path":"a"})" });
    req.messages.push_back(assistant);
    req.messages.push_back(
        { imza::Message::Type::TOOL, "file body", { }, "call_1" });

    const imza::JsonValue v = wire(p.build(req));
    REQUIRE(len(at(v, "tools")) == 1);
    const imza::JsonValue& tool_spec = idx(*at(v, "tools"), 0);
    CHECK(str(at(tool_spec, "type")) == "function");
    CHECK(str(at(*at(tool_spec, "function"), "name")) == "read");
    CHECK(str(at(*at(tool_spec, "function"), "description")) == "read a file");
    const imza::JsonValue* parameters
        = at(*at(tool_spec, "function"), "parameters");
    REQUIRE(parameters != nullptr);
    CHECK(has(*at(*parameters, "properties"), "path"));
    CHECK(str(at(v, "tool_choice")) == "auto");

    const imza::JsonValue& asst = idx(*at(v, "messages"), 0);
    CHECK(str(at(asst, "role")) == "assistant");
    REQUIRE(len(at(asst, "tool_calls")) == 1);
    const imza::JsonValue& call = idx(*at(asst, "tool_calls"), 0);
    CHECK(str(at(call, "id")) == "call_1");
    CHECK(str(at(*at(call, "function"), "name")) == "read");
    CHECK(str(at(*at(call, "function"), "arguments")) == R"({"path":"a"})");

    const imza::JsonValue& tool = idx(*at(v, "messages"), 1);
    CHECK(str(at(tool, "role")) == "tool");
    CHECK(str(at(tool, "content")) == "file body");
    CHECK(str(at(tool, "tool_call_id")) == "call_1");
}

TEST_CASE("Anthropic serializes tool specs and tool_result blocks")
{
    imza::Route route;
    route.dialect = imza::ApiStandard::ANTHROPIC;
    const auto p  = imza::get_provider(route);

    imza::ToolSpec spec;
    spec.name        = "grep";
    spec.description = "search files";
    spec.parameters
        = R"({"type":"object","properties":{"pattern":{"type":"string"}}})";

    imza::ChatRequest req;
    req.model = "claude";
    req.tools = { spec };
    imza::Message assistant { imza::Message::Type::ASSISTANT, "looking" };
    assistant.tool_calls.push_back({ "tu_1", "grep", R"({"pattern":"foo"})" });
    req.messages.push_back(assistant);
    req.messages.push_back(
        { imza::Message::Type::TOOL, "2 matches", { }, "tu_1" });

    const imza::JsonValue v = wire(p.build(req));
    REQUIRE(len(at(v, "tools")) == 1);
    const imza::JsonValue& tool_spec = idx(*at(v, "tools"), 0);
    CHECK(str(at(tool_spec, "name")) == "grep");
    const imza::JsonValue* schema = at(tool_spec, "input_schema");
    REQUIRE(schema != nullptr);
    CHECK(has(*at(*schema, "properties"), "pattern"));

    const imza::JsonValue& asst          = idx(*at(v, "messages"), 0);
    const imza::JsonValue* content_value = at(asst, "content");
    REQUIRE(content_value != nullptr);
    REQUIRE(content_value->is_array());
    const auto& content = content_value->get<imza::JsonValue::array_t>();
    REQUIRE(content.size() == 2);
    CHECK(str(at(content[0], "type")) == "text");
    CHECK(str(at(content[0], "text")) == "looking");
    CHECK(str(at(content[1], "type")) == "tool_use");
    CHECK(str(at(content[1], "id")) == "tu_1");
    CHECK(str(at(*at(content[1], "input"), "pattern")) == "foo");

    const imza::JsonValue& result = idx(*at(v, "messages"), 1);
    CHECK(str(at(result, "role")) == "user");
    const imza::JsonValue* result_content = at(result, "content");
    REQUIRE(result_content != nullptr);
    REQUIRE(result_content->is_array());
    const auto& blocks = result_content->get<imza::JsonValue::array_t>();
    REQUIRE(blocks.size() == 1);
    CHECK(str(at(blocks[0], "type")) == "tool_result");
    CHECK(str(at(blocks[0], "tool_use_id")) == "tu_1");
    CHECK(str(at(blocks[0], "content")) == "2 matches");
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

    REQUIRE(outs.size() == 7);
    CHECK(outs[0].kind == imza::StreamEvent::Kind::REASONING);
    CHECK(outs[0].text == "Thinking");
    CHECK(outs[1].kind == imza::StreamEvent::Kind::CONTENT_DELTA);
    CHECK(outs[1].text == "Hello");
    CHECK(outs[2].kind == imza::StreamEvent::Kind::TOOL_CALL_START);
    CHECK(outs[2].tool_call.id == "call_7");
    CHECK(outs[2].tool_call.name == "read");
    CHECK(outs[3].kind == imza::StreamEvent::Kind::TOOL_CALL);
    CHECK(outs[3].tool_call.id == "call_7");
    CHECK(outs[3].tool_call.name == "read");
    CHECK(outs[3].tool_call.args == R"({"path":"a"})");
    CHECK(outs[4].kind == imza::StreamEvent::Kind::REASONING);
    CHECK(outs[4].thinking_signature == "secret");
    CHECK(outs[5].kind == imza::StreamEvent::Kind::USAGE);
    CHECK(outs[5].usage.prompt == 100);
    CHECK(outs[5].usage.completion == 20);
    CHECK(outs[5].usage.cached_read == 60);
    CHECK(outs[5].usage.cached_write == 5);
    CHECK(outs[6].kind == imza::StreamEvent::Kind::DONE);
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
    CHECK(outs[0].kind == imza::StreamEvent::Kind::TOOL_CALL_START);
    CHECK(outs[0].tool_call.id == "call_a");
    CHECK(outs[0].tool_call.name == "read");
    CHECK(outs[1].kind == imza::StreamEvent::Kind::CONTENT_DELTA);
    CHECK(outs[2].kind == imza::StreamEvent::Kind::TOOL_CALL_START);
    CHECK(outs[2].tool_call.id == "call_b");
    CHECK(outs[2].tool_call.name == "grep");
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

    REQUIRE(outs.size() == 3);
    CHECK(outs[0].kind == imza::StreamEvent::Kind::TOOL_CALL_START);
    CHECK(outs[0].tool_call.id == "tu_9");
    CHECK(outs[0].tool_call.name == "write");
    CHECK(outs[1].kind == imza::StreamEvent::Kind::TOOL_CALL);
    CHECK(outs[1].tool_call.id == "tu_9");
    CHECK(outs[1].tool_call.name == "write");
    CHECK(outs[1].tool_call.args == R"({"path":"b.txt"})");
    CHECK(outs[2].kind == imza::StreamEvent::Kind::DONE);
}

TEST_CASE("OpenAI requests include_usage and emits a single usage event")
{
    const auto p = imza::get_provider(imza::Route { });

    const imza::JsonValue built
        = wire(p.build(imza::ChatRequest { "gpt-4o", { }, { } }));
    CHECK(truthy(at(*at(built, "stream_options"), "include_usage")));

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

TEST_CASE("classify_failure maps statuses onto retry categories")
{
    std::string message;
    CHECK(imza::classify_failure(
              429, R"({"error":{"message":"slow down"}})", message)
        == imza::Status::RATE_LIMITED);
    CHECK(message == "slow down");
    CHECK(imza::classify_failure(402, "", message)
        == imza::Status::BUDGET_EXCEEDED);
    CHECK(
        imza::classify_failure(408, "", message) == imza::Status::SERVER_ERROR);
    CHECK(
        imza::classify_failure(409, "", message) == imza::Status::SERVER_ERROR);
    CHECK(
        imza::classify_failure(500, "", message) == imza::Status::SERVER_ERROR);
    CHECK(
        imza::classify_failure(503, "", message) == imza::Status::SERVER_ERROR);
    CHECK(imza::classify_failure(400, "", message) == imza::Status::API_ERROR);
    CHECK(message == "HTTP 400");
}

TEST_CASE("Anthropic emits usage and content across the message lifecycle")
{
    imza::Route route;
    route.dialect = imza::ApiStandard::ANTHROPIC;
    const auto p  = imza::get_provider(route);

    imza::ParseState state;
    const auto outs = parse_all(p, state,
        { { "message_start",
              R"({"type":"message_start","message":{"usage":{"input_tokens":10,"cache_read_input_tokens":60,"cache_creation_input_tokens":30}}})" },
            { "content_block_delta",
                R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"ok"}})" },
            { "message_delta",
                R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":7}})" },
            { "message_stop", R"({"type":"message_stop"})" } });
    REQUIRE(outs.size() == 3);
    CHECK(outs[0].kind == imza::StreamEvent::Kind::CONTENT_DELTA);
    CHECK(outs[0].text == "ok");
    CHECK(outs[1].kind == imza::StreamEvent::Kind::USAGE);
    CHECK(outs[1].usage.cached_read == 60);
    CHECK(outs[1].usage.cached_write == 30);
    CHECK(outs[1].usage.prompt == 100);
    CHECK(outs[1].usage.completion == 7);
    CHECK(outs[1].usage.total == 107);
    CHECK(outs[2].kind == imza::StreamEvent::Kind::DONE);
    CHECK(state.terminal);
}

TEST_CASE("base64 encodes raw bytes with standard padding")
{
    CHECK(imza::base64_encode("").empty());
    CHECK(imza::base64_encode("f") == "Zg==");
    CHECK(imza::base64_encode("fo") == "Zm8=");
    CHECK(imza::base64_encode("foo") == "Zm9v");
    CHECK(imza::base64_encode(std::string("\0\xff", 2)) == "AP8=");
}

TEST_CASE("base64 decodes padded input and rejects malformed data")
{
    CHECK(imza::base64_decode("").value_or("x").empty());
    CHECK(imza::base64_decode("Zg==").value_or("?") == "f");
    CHECK(imza::base64_decode("Zm8=").value_or("?") == "fo");
    CHECK(imza::base64_decode("Zm9v").value_or("?") == "foo");
    CHECK(
        imza::base64_decode("AP8=").value_or("?") == std::string("\0\xff", 2));

    CHECK_FALSE(imza::base64_decode("Zm9vY").has_value());
    CHECK_FALSE(imza::base64_decode("Zm9vY2Fk!").has_value());
    CHECK_FALSE(imza::base64_decode("Zm9=Y2F0").has_value());
    CHECK_FALSE(imza::base64_decode("Z=====").has_value());
}

TEST_CASE("base64 round-trips every byte value")
{
    std::string bytes;
    for (int i = 0; i < 256; ++i) {
        bytes += static_cast<char>(i);
    }
    CHECK(
        imza::base64_decode(imza::base64_encode(bytes)).value_or("?") == bytes);
}

TEST_CASE("media data url reuses the cached base64 payload")
{
    imza::Attachment media { "photo.png", "cat", imza::Attachment::Type::IMAGE,
        "image/png" };
    CHECK(imza::media_data_url(media) == "data:image/png;base64,Y2F0");

    media.encoded = "TUVET0NLRUQ=";
    CHECK(imza::media_data_url(media) == "data:image/png;base64,TUVET0NLRUQ=");
}

TEST_CASE("OpenAI Chat serializes user images and PDFs")
{
    const auto provider = imza::get_provider(imza::Route { });
    imza::ChatRequest request;
    request.model = "gpt-4o";
    imza::Message user { imza::Message::Type::USER, "inspect" };
    user.media.push_back(
        { "photo.png", "cat", imza::Attachment::Type::IMAGE, "image/png" });
    user.media.push_back({ "report.pdf", std::string("\xff\0", 2),
        imza::Attachment::Type::PDF, "application/pdf" });
    request.messages.push_back(std::move(user));

    const imza::JsonValue value = wire(provider.build(request));
    const imza::JsonValue* content
        = at(idx(*at(value, "messages"), 0), "content");
    REQUIRE(content != nullptr);
    REQUIRE(content->is_array());
    const auto& blocks = content->get<imza::JsonValue::array_t>();
    REQUIRE(blocks.size() == 3);
    CHECK(str(at(blocks[0], "type")) == "text");
    CHECK(str(at(blocks[0], "text")) == "inspect");
    CHECK(str(at(blocks[1], "type")) == "image_url");
    CHECK(str(at(*at(blocks[1], "image_url"), "url"))
        == "data:image/png;base64,Y2F0");
    CHECK(str(at(blocks[2], "type")) == "file");
    CHECK(str(at(*at(blocks[2], "file"), "filename")) == "report.pdf");
    CHECK(str(at(*at(blocks[2], "file"), "file_data"))
        == "data:application/pdf;base64,/wA=");
}

TEST_CASE("OpenAI Responses serializes user images and PDFs")
{
    imza::Route route;
    route.dialect       = imza::ApiStandard::OPENAI_RESPONSES;
    const auto provider = imza::get_provider(route);
    imza::ChatRequest request;
    request.model = "gpt-5";
    imza::Message user { imza::Message::Type::USER, "inspect" };
    user.media.push_back(
        { "photo.png", "cat", imza::Attachment::Type::IMAGE, "image/png" });
    user.media.push_back({ "report.pdf", std::string("\xff\0", 2),
        imza::Attachment::Type::PDF, "application/pdf" });
    request.messages.push_back(std::move(user));

    const imza::JsonValue value    = wire(provider.build(request));
    const imza::JsonValue* content = at(idx(*at(value, "input"), 0), "content");
    REQUIRE(content != nullptr);
    REQUIRE(content->is_array());
    const auto& blocks = content->get<imza::JsonValue::array_t>();
    REQUIRE(blocks.size() == 3);
    CHECK(str(at(blocks[0], "type")) == "input_text");
    CHECK(str(at(blocks[0], "text")) == "inspect");
    CHECK(str(at(blocks[1], "type")) == "input_image");
    CHECK(str(at(blocks[1], "image_url")) == "data:image/png;base64,Y2F0");
    CHECK(str(at(blocks[2], "type")) == "input_file");
    CHECK(str(at(blocks[2], "filename")) == "report.pdf");
    CHECK(
        str(at(blocks[2], "file_data")) == "data:application/pdf;base64,/wA=");
}

TEST_CASE("Anthropic serializes user images and PDFs")
{
    imza::Route route;
    route.dialect       = imza::ApiStandard::ANTHROPIC;
    const auto provider = imza::get_provider(route);
    imza::ChatRequest request;
    request.model = "claude";
    imza::Message user { imza::Message::Type::USER, "inspect" };
    user.media.push_back(
        { "photo.png", "cat", imza::Attachment::Type::IMAGE, "image/png" });
    user.media.push_back({ "report.pdf", std::string("\xff\0", 2),
        imza::Attachment::Type::PDF, "application/pdf" });
    request.messages.push_back(std::move(user));

    const imza::JsonValue value = wire(provider.build(request));
    const imza::JsonValue* content
        = at(idx(*at(value, "messages"), 0), "content");
    REQUIRE(content != nullptr);
    REQUIRE(content->is_array());
    const auto& blocks = content->get<imza::JsonValue::array_t>();
    REQUIRE(blocks.size() == 3);
    CHECK(str(at(blocks[0], "type")) == "text");
    CHECK(str(at(blocks[0], "text")) == "inspect");
    CHECK(str(at(blocks[1], "type")) == "image");
    CHECK(str(at(*at(blocks[1], "source"), "type")) == "base64");
    CHECK(str(at(*at(blocks[1], "source"), "media_type")) == "image/png");
    CHECK(str(at(*at(blocks[1], "source"), "data")) == "Y2F0");
    CHECK(str(at(blocks[2], "type")) == "document");
    CHECK(str(at(*at(blocks[2], "source"), "type")) == "base64");
    CHECK(str(at(*at(blocks[2], "source"), "media_type")) == "application/pdf");
    CHECK(str(at(*at(blocks[2], "source"), "data")) == "/wA=");
}

TEST_CASE(
    "providers keep scalar content and ignore media on assistant messages")
{
    imza::ChatRequest request;
    request.model = "model";
    imza::Message assistant { imza::Message::Type::ASSISTANT, "answer" };
    assistant.media.push_back(
        { "ignored.png", "cat", imza::Attachment::Type::IMAGE, "image/png" });
    request.messages.push_back(std::move(assistant));

    const imza::JsonValue chat
        = wire(imza::get_provider(imza::Route { }).build(request));
    CHECK(at(idx(*at(chat, "messages"), 0), "content")->is_string());
    CHECK(str(at(idx(*at(chat, "messages"), 0), "content")) == "answer");

    imza::Route responses_route;
    responses_route.dialect = imza::ApiStandard::OPENAI_RESPONSES;
    const imza::JsonValue responses
        = wire(imza::get_provider(responses_route).build(request));
    CHECK(at(idx(*at(responses, "input"), 0), "content")->is_string());
    CHECK(str(at(idx(*at(responses, "input"), 0), "content")) == "answer");

    imza::Route anthropic_route;
    anthropic_route.dialect = imza::ApiStandard::ANTHROPIC;
    const imza::JsonValue anthropic
        = wire(imza::get_provider(anthropic_route).build(request));
    CHECK(at(idx(*at(anthropic, "messages"), 0), "content")->is_string());
    CHECK(str(at(idx(*at(anthropic, "messages"), 0), "content")) == "answer");
}

TEST_CASE("json_parse_checked reports errors and succeeds cleanly")
{
    JsonTestPayload payload;
    const glz::error_ctx ok
        = imza::json_parse_checked(R"({"name":"a","count":2})", payload);
    CHECK(!ok);
    CHECK(payload.name == "a");
    CHECK(payload.count == 2);

    const std::string_view broken = R"({"name":)";
    const glz::error_ctx bad      = imza::json_parse_checked(broken, payload);
    CHECK(bad);
    const std::string diagnostic = imza::json_parse_error(broken, bad);
    CHECK(!diagnostic.empty());
}

TEST_CASE("json_parse rejects type mismatches for reflected structs")
{
    JsonTestPayload payload;
    payload.count = 5;
    CHECK(!imza::json_parse(R"({"count":"x"})", payload));
    CHECK(imza::json_parse(R"({"count":4})", payload));
    CHECK(payload.count == 4);
}

TEST_CASE("json_dump_checked round-trips")
{
    const JsonTestPayload value { "a", 3, 7 };
    const std::string text = imza::json_dump(value);
    CHECK(text == R"({"name":"a","count":3,"note":7})");

    const JsonTestPayload bare { "b", 1, std::nullopt };
    CHECK(imza::json_dump(bare) == R"({"name":"b","count":1})");

    const std::optional<std::string> pretty
        = imza::json_dump_pretty_checked(bare);
    REQUIRE(pretty.has_value());
    CHECK(pretty->find("\n  ") != std::string::npos);
}

TEST_CASE("tool schemas embed verbatim without re-validation")
{
    const auto p = imza::get_provider(imza::Route { });
    imza::ToolSpec spec;
    spec.name        = "weird";
    spec.description = "schema is not validated by imza";
    // Not schema-valid JSON, but imza forwards it untouched either way.
    spec.parameters = R"({"type":"weird","x":1})";

    imza::ChatRequest req;
    req.model = "gpt-4o";
    req.tools = { spec };

    const imza::JsonValue v = wire(p.build(req));
    const imza::JsonValue& parameters
        = *at(*at(idx(*at(v, "tools"), 0), "function"), "parameters");
    // Embedded as an object, not a JSON string of the schema.
    CHECK(parameters.is_object());
    CHECK(str(at(parameters, "type")) == "weird");

    imza::Route route;
    route.dialect = imza::ApiStandard::ANTHROPIC;
    const imza::JsonValue anthropic
        = wire(imza::get_provider(route).build(req));
    const imza::JsonValue& input_schema
        = *at(idx(*at(anthropic, "tools"), 0), "input_schema");
    CHECK(input_schema.is_object());
    CHECK(str(at(input_schema, "type")) == "weird");
}
