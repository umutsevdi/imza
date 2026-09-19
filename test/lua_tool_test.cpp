#include <doctest/doctest.h>
#include <json/json.h>
#include <string>
#include <utility>

#include "network/json_io.h"
#include "tools/tool.h"

namespace {

imza::ToolOutput run_script(std::string script)
{
    const imza::Tool tool = imza::make_lua_tool();
    imza::ToolCallRequest req;
    req.name = "lua";
    Json::Value args;
    args["script"] = std::move(script);
    req.args       = imza::write_json(args);
    return imza::dispatch_tool({ &tool, 1 }, req);
}

} // namespace

TEST_CASE("lua tool prints values to its output")
{
    const imza::ToolOutput out = run_script("print('hello', 42, true)\n"
                                            "local t = {3, 1, 2}\n"
                                            "table.sort(t)\n"
                                            "print(table.concat(t, ','))");
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text == "hello\t42\ttrue\n1,2,3\n");
}

TEST_CASE("lua tool rejects empty scripts and reports errors with positions")
{
    CHECK(run_script("").kind == imza::ToolOutput::Kind::ERROR);

    const imza::ToolOutput runtime = run_script("error('boom')");
    CHECK(runtime.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(runtime.text.find("boom") != std::string::npos);

    const imza::ToolOutput syntax = run_script("retuern 1");
    CHECK(syntax.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(syntax.text.find(":1:") != std::string::npos);
}

TEST_CASE("lua sandbox denies filesystem, os, and byte-code access")
{
    CHECK(run_script("io.read()").text.find("attempt to index a nil value")
        != std::string::npos);

    CHECK(run_script("os.execute('touch /tmp/imza_pwn')").kind
        == imza::ToolOutput::Kind::ERROR);

    const imza::ToolOutput chunk
        = run_script("print(load('\\27Lua binary') == nil)");
    CHECK(chunk.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(chunk.text.find("true") != std::string::npos);
}

TEST_CASE("lua tool kills runaway loops at the deadline")
{
    const imza::Tool tool = imza::make_lua_tool();
    imza::ToolCallRequest req;
    req.name = "lua";
    req.args = R"({"script":"while true do end","timeout":1})";
    const imza::ToolOutput out = imza::dispatch_tool({ &tool, 1 }, req);
    CHECK(out.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(out.text.find("time limit") != std::string::npos);
}

TEST_CASE("lua tool truncates oversized output")
{
    imza::ToolCallRequest req;
    req.name              = "lua";
    req.args              = R"({"script":"for i = 1, 100000 do print(i) end"})";
    const imza::Tool tool = imza::make_lua_tool();
    const imza::ToolOutput out = imza::dispatch_tool({ &tool, 1 }, req);
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text.size() < 100 * 1024);
    CHECK(out.text.find("[truncated]") != std::string::npos);
}

TEST_CASE("default_tools includes lua in every mode")
{
    const auto tools = imza::default_tools(imza::RuntimeFlag::NONE);
    CHECK(imza::find_tool(tools, "lua") != nullptr);
}
