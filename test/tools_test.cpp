#include <doctest/doctest.h>
#include <json/json.h>

#include "network/json_io.h"
#include "tools/tool.h"

namespace imza {

namespace {

    std::vector<Tool> echo_tools()
    {
        return {
            { { "echo", "echo the message",
                  parse_json(
                      R"({"type":"object","properties":{"msg":{"type":"string"}}})") },
                [](const ToolCallRequest&, const Json::Value& args) {
                    return ToolOutput { ToolOutput::Kind::OUTPUT,
                        args.get("msg", "").asString() };
                } }
        };
    }

} // namespace

TEST_CASE("dispatch parses object args for the handler")
{
    const std::vector<Tool> tools = echo_tools();
    const ToolOutput out          = dispatch_tool(
        tools, ToolCallRequest { "echo", R"({"msg":"hi"})", "", "" });
    CHECK(out.kind == ToolOutput::Kind::OUTPUT);
    CHECK(out.text == "hi");
}

TEST_CASE("dispatch passes non-JSON args through as a string value")
{
    std::vector<Tool> tools {
        { { "raw", "takes raw text", Json::Value(Json::objectValue) },
            [](const ToolCallRequest&, const Json::Value& args) {
                return ToolOutput { ToolOutput::Kind::OUTPUT, args.asString() };
            } }
    };
    const ToolOutput out = dispatch_tool(tools, { "raw", "ls -la", "", "" });
    CHECK(out.kind == ToolOutput::Kind::OUTPUT);
    CHECK(out.text == "ls -la");
}

TEST_CASE("dispatch propagates handler errors")
{
    std::vector<Tool> tools {
        { { "boom", "always fails", Json::Value(Json::objectValue) },
            [](const ToolCallRequest&, const Json::Value&) {
                return ToolOutput { ToolOutput::Kind::ERROR, "it broke" };
            } }
    };
    const ToolOutput out = dispatch_tool(tools, { "boom", "{}", "", "" });
    CHECK(out.kind == ToolOutput::Kind::ERROR);
    CHECK(out.text == "it broke");
}

} // namespace imza
