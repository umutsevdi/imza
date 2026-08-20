#include <doctest/doctest.h>
#include <json/json.h>

#include "network.hpp"
#include "types.hpp"

TEST_CASE("OpenAI request shape via factory")
{
    ursa::Config cfg;
    cfg.standard = ursa::ApiStandard::OPENAI;

    const auto p = ursa::get_provider(cfg);

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
    CHECK(p.endpoint() == "/chat/completions");
}

TEST_CASE("Anthropic request shape via factory")
{
    ursa::Config cfg;
    cfg.standard = ursa::ApiStandard::ANTHROPIC;

    const auto p = ursa::get_provider(cfg);

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
    CHECK(p.endpoint() == "/v1/messages");
}

TEST_CASE("OpenAI content delta")
{
    ursa::Config cfg;
    cfg.standard = ursa::ApiStandard::OPENAI;
    const auto p = ursa::get_provider(cfg);

    ursa::StreamEvent ev;
    const auto st
        = p.parse("", R"({"choices":[{"delta":{"content":"Hello"}}]})", ev);
    CHECK(st == ursa::Status::OK);
    CHECK(ev.kind == ursa::StreamEvent::Kind::CONTENT_DELTA);
    CHECK(ev.text == "Hello");
}

TEST_CASE("OpenAI done")
{
    ursa::Config cfg;
    cfg.standard = ursa::ApiStandard::OPENAI;
    const auto p = ursa::get_provider(cfg);

    ursa::StreamEvent ev;
    const auto st = p.parse("", "[DONE]", ev);
    CHECK(st == ursa::Status::OK);
    CHECK(ev.kind == ursa::StreamEvent::Kind::DONE);
}

TEST_CASE("Anthropic content delta")
{
    ursa::Config cfg;
    cfg.standard = ursa::ApiStandard::ANTHROPIC;
    const auto p = ursa::get_provider(cfg);

    ursa::StreamEvent ev;
    const auto st = p.parse("content_block_delta",
        R"({"type":"content_block_delta","delta":{"type":"text_delta","text":"Hi"}})",
        ev);
    CHECK(st == ursa::Status::OK);
    CHECK(ev.kind == ursa::StreamEvent::Kind::CONTENT_DELTA);
    CHECK(ev.text == "Hi");
}

TEST_CASE("Anthropic stop")
{
    ursa::Config cfg;
    cfg.standard = ursa::ApiStandard::ANTHROPIC;
    const auto p = ursa::get_provider(cfg);

    ursa::StreamEvent ev;
    const auto st = p.parse("message_stop", "{}", ev);
    CHECK(st == ursa::Status::OK);
    CHECK(ev.kind == ursa::StreamEvent::Kind::DONE);
}

TEST_CASE("get_provider selects by standard")
{
    ursa::Config openai;
    openai.standard = ursa::ApiStandard::OPENAI;
    CHECK(ursa::get_provider(openai).endpoint() == "/chat/completions");

    ursa::Config anthropic;
    anthropic.standard = ursa::ApiStandard::ANTHROPIC;
    CHECK(ursa::get_provider(anthropic).endpoint() == "/v1/messages");
}
