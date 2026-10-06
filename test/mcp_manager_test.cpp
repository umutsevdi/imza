#include <doctest/doctest.h>

#include "loopback_mcp_server.h"
#include "network/mcp.h"
#include "tools/mcp_manager.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

imza::McpServerConfig server_config(
    const std::string& id, const std::string& url)
{
    imza::McpServerConfig config;
    config.id  = id;
    config.url = url;
    return config;
}

// Connect/disconnect run on manager workers; poll for the transition.
bool wait_state(
    imza::McpManager& manager, const std::string& id, imza::McpServerState want)
{
    for (int i = 0; i < 500; ++i) {
        for (const auto& server : manager.snapshot()) {
            if (server.id == id && server.state == want) {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

template <typename Predicate> bool wait_for(Predicate predicate)
{
    for (int i = 0; i < 500; ++i) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

} // namespace

TEST_CASE("mcp manager connects, caches tools, and calls through the session")
{
    imza::test::allow_loopback_direct();
    imza::test::LoopbackMcpServer server;
    REQUIRE(server.start());

    imza::McpManager manager({ { "exa", server_config("exa", server.url()) } });
    unsigned changes = 0;
    imza::Signal<>::Subscription subscription
        = manager.subscribe([&] { ++changes; });

    auto snapshot = manager.snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot[0].id == "exa");
    CHECK(snapshot[0].label == "exa"); // label defaults to the id
    CHECK(snapshot[0].state == imza::McpServerState::OFFLINE);

    manager.connect("exa");
    REQUIRE(wait_state(manager, "exa", imza::McpServerState::CONNECTED));
    snapshot = manager.snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot[0].state == imza::McpServerState::CONNECTED);
    CHECK(snapshot[0].detail.empty());
    CHECK(snapshot[0].tool_count == 3); // two pages of the fixture inventory
    CHECK(changes >= 2);

    const auto tools = manager.tools("exa");
    REQUIRE(tools.has_value());
    REQUIRE(tools->size() == 3);
    CHECK((*tools)[0].name == "echo");

    imza::McpToolCallResult result;
    std::string detail;
    imza::JsonValue arguments;
    arguments["query"] = "anything";
    REQUIRE(manager.call("exa", "echo", arguments, result, detail)
        == imza::Status::OK);
    CHECK(imza::mcp_first_text(result) == "loopback ok");

    manager.disconnect("exa");
    snapshot = manager.snapshot();
    CHECK(snapshot[0].state == imza::McpServerState::OFFLINE);
    CHECK(snapshot[0].tool_count == 0);
    CHECK_FALSE(manager.tools("exa").has_value());

    CHECK(wait_for([&] { return server.saw_delete.load(); }));
    server.stop();
    CHECK(server.session_echo_ok.load());
    CHECK(server.protocol_header_ok.load());
}

TEST_CASE("mcp manager reports connection failures and gates calls")
{
    // Port 1 on loopback refuses immediately.
    imza::McpManager manager(
        { { "dead", server_config("dead", "http://127.0.0.1:1/mcp") } });

    manager.connect("dead");
    REQUIRE(wait_state(manager, "dead", imza::McpServerState::FAILED));
    const auto snapshot = manager.snapshot();
    CHECK(snapshot[0].state == imza::McpServerState::FAILED);
    CHECK_FALSE(snapshot[0].detail.empty());

    imza::McpToolCallResult result;
    std::string detail;
    CHECK(manager.call("dead", "echo", { }, result, detail)
        == imza::Status::CONFIG_ERROR);
    CHECK(detail.find("not connected") != std::string::npos);

    CHECK(manager.call("missing", "echo", { }, result, detail)
        == imza::Status::CONFIG_ERROR);
    CHECK(detail.find("unknown mcp server") != std::string::npos);

    manager.connect("missing"); // unknown ids are ignored, not a crash
}

TEST_CASE("mcp manager keeps disabled servers off")
{
    imza::test::allow_loopback_direct();
    imza::test::LoopbackMcpServer server;
    REQUIRE(server.start());

    imza::McpServerConfig config = server_config("off", server.url());
    config.enabled               = false;
    imza::McpManager manager({ { "off", config } });

    auto snapshot = manager.snapshot();
    CHECK(snapshot[0].state == imza::McpServerState::DISABLED);

    manager.connect("off"); // no-op, not an error
    snapshot = manager.snapshot();
    CHECK(snapshot[0].state == imza::McpServerState::DISABLED);

    imza::McpToolCallResult result;
    std::string detail;
    CHECK(manager.call("off", "echo", { }, result, detail)
        == imza::Status::CONFIG_ERROR);
    CHECK(detail.find("disabled") != std::string::npos);
    CHECK_FALSE(manager.tools("off").has_value());

    server.stop();
    CHECK_FALSE(server.saw_delete.load());
}

TEST_CASE("mcp manager serializes concurrent calls on one session")
{
    imza::test::allow_loopback_direct();
    imza::test::LoopbackMcpServer server;
    REQUIRE(server.start());

    imza::McpManager manager({ { "exa", server_config("exa", server.url()) } });
    manager.connect("exa");

    std::atomic<int> ok = 0;
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&] {
            for (int i = 0; i < 5; ++i) {
                imza::McpToolCallResult result;
                std::string detail;
                imza::JsonValue arguments;
                arguments["i"] = static_cast<double>(i);
                if (manager.call("exa", "echo", arguments, result, detail)
                    == imza::Status::OK) {
                    ++ok;
                }
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    CHECK(ok == 20);

    server.stop();
}
