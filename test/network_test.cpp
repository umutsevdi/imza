#include <doctest/doctest.h>
#include <json/json.h>

#include "network.hpp"
#include "types.hpp"

TEST_CASE("build_request OpenAI shape")
{
    ursa::ChatRequest req;
    req.model = "gpt-4o";
    req.messages.push_back({ ursa::Message::Type::SYSTEM, "sys" });
    req.messages.push_back({ ursa::Message::Type::USER, "hi" });

    const Json::Value v = ursa::build_request(ursa::Standard::OPENAI, req);
    CHECK(v["model"].asString() == "gpt-4o");
    CHECK(v["stream"].asBool() == true);
    CHECK(v["messages"].size() == 2);
    CHECK(v["messages"][0]["role"].asString() == "system");
    CHECK(v["messages"][1]["role"].asString() == "user");
    CHECK(v["messages"][1]["content"].asString() == "hi");
}

TEST_CASE("build_request Anthropic shape")
{
    ursa::ChatRequest req;
    req.model = "claude";
    req.messages.push_back({ ursa::Message::Type::SYSTEM, "sys" });
    req.messages.push_back({ ursa::Message::Type::USER, "hi" });

    const Json::Value v = ursa::build_request(ursa::Standard::ANTHROPIC, req);
    CHECK(v["model"].asString() == "claude");
    CHECK(v["system"].size() == 1);
    CHECK(v["system"][0].asString() == "sys");
    CHECK(v["messages"].size() == 1);
    CHECK(v["messages"][0]["role"].asString() == "user");
    CHECK(v.isMember("max_tokens"));
}

TEST_CASE("parse_stream_event OpenAI content delta")
{
    ursa::StreamEvent ev;
    const auto st = ursa::parse_stream_event(ursa::Standard::OPENAI, "",
        R"({"choices":[{"delta":{"content":"Hello"}}]})", ev);
    CHECK(st == ursa::Status::OK);
    CHECK(ev.kind == ursa::StreamEvent::Kind::CONTENT_DELTA);
    CHECK(ev.text == "Hello");
}

TEST_CASE("parse_stream_event OpenAI done")
{
    ursa::StreamEvent ev;
    const auto st
        = ursa::parse_stream_event(ursa::Standard::OPENAI, "", "[DONE]", ev);
    CHECK(st == ursa::Status::OK);
    CHECK(ev.kind == ursa::StreamEvent::Kind::DONE);
}

TEST_CASE("parse_stream_event Anthropic content delta")
{
    ursa::StreamEvent ev;
    const auto st = ursa::parse_stream_event(ursa::Standard::ANTHROPIC,
        "content_block_delta",
        R"({"type":"content_block_delta","delta":{"type":"text_delta","text":"Hi"}})",
        ev);
    CHECK(st == ursa::Status::OK);
    CHECK(ev.kind == ursa::StreamEvent::Kind::CONTENT_DELTA);
    CHECK(ev.text == "Hi");
}

TEST_CASE("parse_stream_event Anthropic stop")
{
    ursa::StreamEvent ev;
    const auto st = ursa::parse_stream_event(
        ursa::Standard::ANTHROPIC, "message_stop", "{}", ev);
    CHECK(st == ursa::Status::OK);
    CHECK(ev.kind == ursa::StreamEvent::Kind::DONE);
}
