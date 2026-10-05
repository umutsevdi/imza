#include "tools/tool.h"
#include <doctest/doctest.h>

namespace imza {

namespace {

    std::vector<Tool> echo_tools()
    {
        return {
            { { "echo", "echo the message",
                  R"({"type":"object","properties":{"msg":{"type":"string"}}})" },
                [](const ToolCallRequest& req) {
                    return ToolOutput { ToolOutput::Kind::OUTPUT, req.args };
                } }
        };
    }

} // namespace

TEST_CASE("dispatch hands the raw args to the handler")
{
    const std::vector<Tool> tools = echo_tools();
    const ToolOutput out          = dispatch_tool(
        tools, ToolCallRequest { "echo", R"({"msg":"hi"})", "", "" });
    CHECK(out.kind == ToolOutput::Kind::OUTPUT);
    CHECK(out.text == R"({"msg":"hi"})");
}

TEST_CASE("dispatch propagates handler errors")
{
    std::vector<Tool> tools { { { "boom", "always fails", "{}" },
        [](const ToolCallRequest&) {
            return ToolOutput { ToolOutput::Kind::ERROR, "it broke" };
        } } };
    const ToolOutput out = dispatch_tool(tools, { "boom", "{}", "", "" });
    CHECK(out.kind == ToolOutput::Kind::ERROR);
    CHECK(out.text == "it broke");
}

} // namespace imza