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

TEST_CASE("dispatch routes by name and reports handler outcomes")
{
    const std::vector<Tool> tools = echo_tools();
    const ToolOutput echoed       = dispatch_tool(
        tools, ToolCallRequest { "echo", R"({"msg":"hi"})", "", "" });
    CHECK(echoed.kind == ToolOutput::Kind::OUTPUT);
    CHECK(echoed.text == R"({"msg":"hi"})");

    // An unregistered name is an error, not a silent no-op.
    const ToolOutput unknown = dispatch_tool(tools, { "nope", "{}", "", "" });
    CHECK(unknown.kind == ToolOutput::Kind::ERROR);
    CHECK(unknown.text.find("nope") != std::string::npos);

    std::vector<Tool> failing { { { "boom", "always fails", "{}" },
        [](const ToolCallRequest&) {
            return ToolOutput { ToolOutput::Kind::ERROR, "it broke" };
        } } };
    const ToolOutput out = dispatch_tool(failing, { "boom", "{}", "", "" });
    CHECK(out.kind == ToolOutput::Kind::ERROR);
    CHECK(out.text == "it broke");
}

} // namespace imza
