#include <doctest/doctest.h>
#include <json/json.h>

#include "tools.h"

namespace ursa {

namespace {

    ToolRegistry echo_registry()
    {
        ToolRegistry tools;
        tools.add({ { "echo", "echo the message",
                        parse_json(
                            R"({"type":"object","properties":{"msg":{"type":"string"}}})") },
            [](const Json::Value& args) {
                return ToolOutput { ToolOutput::Kind::OUTPUT,
                    args.get("msg", "").asString() };
            } });
        return tools;
    }

} // namespace

TEST_CASE("registry finds and lists specs")
{
    const ToolRegistry tools = echo_registry();
    REQUIRE(tools.tools().size() == 1);

    const Tool* found = tools.find("echo");
    REQUIRE(found != nullptr);
    CHECK(found->spec.name == "echo");
    CHECK(found->spec.description == "echo the message");
    CHECK(found->spec.parameters["properties"].isMember("msg"));
    CHECK(found->safety == ToolSafety::MUTATING);
    CHECK(tools.find("missing") == nullptr);

    const auto specs = tools.specs();
    REQUIRE(specs.size() == 1);
    CHECK(specs[0].name == "echo");
}

TEST_CASE("tool safety defaults to mutating and can be overridden")
{
    Tool mutating;
    mutating.spec.name = "m";
    CHECK(mutating.safety == ToolSafety::MUTATING);

    Tool read_only { { "ro", "", Json::Value(Json::objectValue) },
        [](const Json::Value&) {
            return ToolOutput { ToolOutput::Kind::OUTPUT, "" };
        },
        ToolSafety::READ_ONLY };
    CHECK(read_only.safety == ToolSafety::READ_ONLY);
}

TEST_CASE("dispatch parses object args for the handler")
{
    const ToolRegistry tools = echo_registry();
    const ToolOutput out
        = tools.dispatch(ToolCallRequest { "echo", R"({"msg":"hi"})", "", "" });
    CHECK(out.kind == ToolOutput::Kind::OUTPUT);
    CHECK(out.text == "hi");
}

TEST_CASE("dispatch passes non-JSON args through as a string value")
{
    ToolRegistry tools;
    tools.add({ { "raw", "takes raw text", Json::Value(Json::objectValue) },
        [](const Json::Value& args) {
            return ToolOutput { ToolOutput::Kind::OUTPUT, args.asString() };
        } });
    const ToolOutput out = tools.dispatch({ "raw", "ls -la", "", "" });
    CHECK(out.kind == ToolOutput::Kind::OUTPUT);
    CHECK(out.text == "ls -la");
}

TEST_CASE("dispatch reports unknown tools as errors")
{
    const ToolRegistry tools;
    const ToolOutput out = tools.dispatch({ "nope", "{}", "", "" });
    CHECK(out.kind == ToolOutput::Kind::ERROR);
    CHECK(out.text.find("unknown tool: nope") != std::string::npos);
}

TEST_CASE("dispatch propagates handler errors")
{
    ToolRegistry tools;
    tools.add({ { "boom", "always fails", Json::Value(Json::objectValue) },
        [](const Json::Value&) {
            return ToolOutput { ToolOutput::Kind::ERROR, "it broke" };
        } });
    const ToolOutput out = tools.dispatch({ "boom", "{}", "", "" });
    CHECK(out.kind == ToolOutput::Kind::ERROR);
    CHECK(out.text == "it broke");
}

} // namespace ursa
