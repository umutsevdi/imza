#include <doctest/doctest.h>

#include "loopback_mcp_server.h"
#include "network/mcp.h"
#include "test_helpers.h"
#include "tools/mcp_manager.h"
#include "tools/tool.h"
#include "turn/prompt.h"

#include <chrono>
#include <map>
#include <string>
#include <thread>

namespace {

// A lua host wired to a manager connected to the loopback server.
struct McpFixture {
    imza::test::LoopbackMcpServer server;
    std::unique_ptr<imza::McpManager> manager;
    bool enabled = true;

    bool start()
    {
        if (!server.start()) {
            return false;
        }
        imza::McpServerConfig config;
        config.id  = "exa";
        config.url = server.url();
        manager    = std::make_unique<imza::McpManager>(
            std::map<std::string, imza::McpServerConfig> { { "exa", config } });
        manager->connect("exa");
        // The handshake runs on a manager worker; wait for the inventory.
        for (int i = 0; i < 1000; ++i) {
            if (manager->tools("exa").has_value()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    imza::LuaHost host()
    {
        imza::LuaHost host;
        host.mcp_enabled = enabled;
        host.mcp = [this] -> imza::McpManager* { return manager.get(); };
        return host;
    }
};

} // namespace

TEST_CASE("imza.mcp.call returns the tool's text through the manager")
{
    McpFixture fx;
    REQUIRE(fx.start());

    const imza::ToolOutput out = imza::test::run_lua(
        "local out = imza.mcp.call('exa', 'echo', {query='hi'})\n"
        "print('got:' .. out)",
        fx.host());
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text.find("got:loopback ok") != std::string::npos);
    // The call lands in the dispatch log under the model-facing path.
    CHECK(out.dispatch_log.size() == 1);
    CHECK(out.dispatch_log[0].binding == "mcp.call");
}

TEST_CASE("imza.mcp.call works without an args table")
{
    McpFixture fx;
    REQUIRE(fx.start());

    const imza::ToolOutput out = imza::test::run_lua(
        "print('got:' .. imza.mcp.call('exa', 'echo'))", fx.host());
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text.find("got:loopback ok") != std::string::npos);
}

TEST_CASE("imza.mcp.call surfaces tool execution errors as aborts")
{
    imza::test::allow_loopback_direct();
    McpFixture fx;
    fx.server.call_text     = "boom";
    fx.server.call_is_error = true;
    REQUIRE(fx.start());

    const imza::ToolOutput out = imza::test::run_lua(
        "imza.mcp.call('exa', 'echo')\nprint('dead')", fx.host());
    CHECK(out.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(out.text.find("tool error from 'exa.echo'") != std::string::npos);
    CHECK(out.text.find("boom") != std::string::npos);
    CHECK(out.text.find("dead") == std::string::npos);

    fx.server.stop();
}

TEST_CASE("imza.mcp.call fails closed without a manager or capability")
{
    // Capability denied: the gate fires before the handler runs.
    McpFixture fx;
    REQUIRE(fx.start());
    fx.enabled                   = false;
    const imza::ToolOutput gated = imza::test::run_lua(
        "imza.mcp.call('exa', 'echo')\nprint('dead')", fx.host());
    CHECK(gated.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(gated.text.find("MCP access is disabled") != std::string::npos);

    // Manager absent (empty accessor): the binding reports unavailability.
    imza::LuaHost host;
    host.mcp_enabled                   = true;
    const imza::ToolOutput unavailable = imza::test::run_lua(
        "imza.mcp.call('exa', 'echo')\nprint('dead')", host);
    CHECK(unavailable.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(unavailable.text.find("not available") != std::string::npos);
}

TEST_CASE("load_mcp renders the server's tool reference")
{
    McpFixture fx;
    REQUIRE(fx.start());

    imza::Tool tool = imza::make_load_mcp_tool(
        { [&fx] -> imza::McpManager* { return fx.manager.get(); } });
    const imza::ToolOutput out
        = tool.run({ "load_mcp", R"({"server":"exa"})", "", "" });
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text.find("MCP server 'exa'") != std::string::npos);
    CHECK(out.text.find("imza.mcp.call(\"exa\"") != std::string::npos);
    CHECK(out.text.find("- echo") != std::string::npos);
    CHECK(out.text.find("Echo back") != std::string::npos);
    CHECK(out.text.find("\"type\":\"object\"") != std::string::npos);

    const imza::ToolOutput unknown
        = tool.run({ "load_mcp", R"({"server":"nope"})", "", "" });
    CHECK(unknown.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(unknown.text.find("unknown server") != std::string::npos);

    const imza::ToolOutput empty_args = tool.run({ "load_mcp", "{ }", "", "" });
    CHECK(empty_args.kind == imza::ToolOutput::Kind::ERROR);
}

TEST_CASE("the <mcps> block lists configured servers with state")
{
    McpFixture fx;
    REQUIRE(fx.start());

    imza::PromptStore prompts;
    const std::string prompt
        = imza::build_subagent_system_prompt(prompts, nullptr, nullptr,
            imza::SubagentRole::RESEARCH, nullptr, fx.manager.get());
    CHECK(prompt.find("<mcps>") != std::string::npos);
    CHECK(prompt.find("- exa [connected, 3 tools]") != std::string::npos);
    CHECK(prompt.find("load_mcp(\"<id>\")") != std::string::npos);
    CHECK(prompt.find("imza.mcp.call") != std::string::npos);

    // Without a manager no block is rendered.
    const std::string bare = imza::build_subagent_system_prompt(
        prompts, nullptr, nullptr, imza::SubagentRole::RESEARCH);
    CHECK(bare.find("<mcps>") == std::string::npos);
}
