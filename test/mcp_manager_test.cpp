#include <doctest/doctest.h>

#include "loopback_mcp_server.h"
#include "network/mcp.h"
#include "tools/mcp_catalog.h"
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

template <typename Predicate> bool wait_for(Predicate predicate)
{
    for (int i = 0; i < 1000; ++i) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

// Connect/disconnect run on manager workers; poll for the transition.
bool wait_state(
    imza::McpManager& manager, const std::string& id, imza::McpServerState want)
{
    return wait_for([&] {
        for (const auto& server : manager.snapshot()) {
            if (server.id == id && server.state == want) {
                return true;
            }
        }
        return false;
    });
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
    // The handshake runs on a manager worker; calls before it completes
    // would fail, so wait for the inventory first.
    for (int i = 0; i < 1000 && !manager.tools("exa").has_value(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(manager.tools("exa").has_value());

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

TEST_CASE("expand_env_vars resolves references with defaults")
{
    ::setenv("IMZA_TEST_TOKEN", "s3cret", 1);
    CHECK(imza::expand_env_vars("${IMZA_TEST_TOKEN}") == "s3cret");
    CHECK(imza::expand_env_vars("Bearer ${IMZA_TEST_TOKEN}!")
        == "Bearer s3cret!");
    CHECK(
        imza::expand_env_vars("${IMZA_TEST_MISSING:-fallback}") == "fallback");
    CHECK(imza::expand_env_vars("https://${IMZA_TEST_MISSING}/x")
        == "https:///x");
    CHECK(imza::expand_env_vars("plain") == "plain");
    CHECK(imza::expand_env_vars("unterminated ${VAR") == "unterminated ${VAR");
}

TEST_CASE("mcp manager retries failed handshakes with backoff")
{
    imza::test::allow_loopback_direct();
    imza::test::LoopbackMcpServer server;
    server.fail_handshakes = 1;
    REQUIRE(server.start());

    imza::McpManager manager({ { "exa", server_config("exa", server.url()) } });
    manager.connect("exa");
    // First handshake answers 500; the retry lands after the 2 s backoff.
    REQUIRE(wait_state(manager, "exa", imza::McpServerState::CONNECTED));

    imza::McpToolCallResult result;
    std::string detail;
    REQUIRE(
        manager.call("exa", "echo", { }, result, detail) == imza::Status::OK);

    server.stop();
}

TEST_CASE("mcp manager refreshes tools on notifications/tools/list_changed")
{
    imza::test::allow_loopback_direct();
    imza::test::LoopbackMcpServer server;
    server.push_list_changed = true;
    REQUIRE(server.start());

    imza::McpManager manager({ { "exa", server_config("exa", server.url()) } });
    manager.connect("exa");
    REQUIRE(wait_state(manager, "exa", imza::McpServerState::CONNECTED));
    // The notification may land before this check runs: the inventory is
    // either the initial roster or the already-refreshed one.
    CHECK(manager.tools("exa").has_value());

    // The notification listener re-lists; the fixture answers with the
    // updated one-tool roster.
    REQUIRE(wait_for([&] {
        const auto tools = manager.tools("exa");
        return tools.has_value() && tools->size() == 1
            && (*tools)[0].name == "changed-tool";
    }));

    server.stop();
}

TEST_CASE("mcp listener exits quietly when the server has no stream")
{
    imza::test::allow_loopback_direct();
    imza::test::LoopbackMcpServer server;
    server.get_returns_405 = true;
    REQUIRE(server.start());

    imza::McpManager manager({ { "exa", server_config("exa", server.url()) } });
    manager.connect("exa");
    REQUIRE(wait_state(manager, "exa", imza::McpServerState::CONNECTED));
    REQUIRE(wait_for([&] { return server.saw_get.load(); }));

    // Still connected, listener gone; calls keep working.
    imza::McpToolCallResult result;
    std::string detail;
    CHECK(manager.call("exa", "echo", { }, result, detail) == imza::Status::OK);
    CHECK(manager.snapshot()[0].state == imza::McpServerState::CONNECTED);

    server.stop();
}

TEST_CASE("mcp listener rebuilds the session after a stream 404")
{
    imza::test::allow_loopback_direct();
    imza::test::LoopbackMcpServer server;
    server.get_returns_404 = true;
    REQUIRE(server.start());

    imza::McpManager manager({ { "exa", server_config("exa", server.url()) } });
    manager.connect("exa");
    REQUIRE(wait_state(manager, "exa", imza::McpServerState::CONNECTED));
    REQUIRE(wait_for([&] { return server.saw_get.load(); }));

    // The expired session must be rebuilt (second initialize) and the
    // entry must keep answering calls; the rebuild path must not hold the
    // entry I/O lock or the call below deadlocks.
    REQUIRE(wait_for([&] { return server.initialize_count.load() >= 2; }));
    imza::McpToolCallResult result;
    std::string detail;
    CHECK(manager.call("exa", "echo", { }, result, detail) == imza::Status::OK);
    REQUIRE(wait_state(manager, "exa", imza::McpServerState::CONNECTED));

    server.stop();
}

TEST_CASE("mcp manager reload drops removed servers and applies toggles")
{
    imza::test::allow_loopback_direct();
    imza::test::LoopbackMcpServer server;
    REQUIRE(server.start());

    imza::McpManager manager({ { "exa", server_config("exa", server.url()) } });
    manager.connect("exa");
    REQUIRE(wait_state(manager, "exa", imza::McpServerState::CONNECTED));

    imza::McpServerConfig toggled = server_config("exa", server.url());
    toggled.enabled               = false;
    manager.reload({ { "exa", toggled } });
    REQUIRE(wait_state(manager, "exa", imza::McpServerState::DISABLED));

    imza::McpServerConfig fresh = server_config("exa", server.url());
    manager.reload({ { "exa", fresh } });
    REQUIRE(wait_state(manager, "exa", imza::McpServerState::OFFLINE));
    CHECK_FALSE(manager.tools("exa").has_value());

    manager.reload({ { "other", server_config("other", server.url()) } });
    CHECK(manager.snapshot().size() == 1);
    CHECK(manager.snapshot()[0].id == "other");

    server.stop();
}

TEST_CASE("the bundled mcp catalogue parses with usable entries")
{
    const auto catalog = imza::load_mcp_catalog();
    REQUIRE_FALSE(catalog.empty());
    bool has_https = false;
    for (const auto& entry : catalog) {
        CHECK_FALSE(entry.id.empty());
        CHECK_FALSE(entry.label.empty());
        CHECK(entry.url.rfind("https://", 0) == 0);
        const bool known_auth = entry.auth_kind == "none"
            || entry.auth_kind == "token" || entry.auth_kind == "oauth";
        CHECK(known_auth);
        if (entry.id == "github") {
            has_https = true;
        }
    }
    CHECK(has_https);
}

TEST_CASE("mcp manager fails catalogue references offline when dangling")
{
    imza::McpServerConfig config;
    config.id         = "gone";
    config.catalog_id = "no-such-entry";
    imza::McpManager manager(
        std::map<std::string, imza::McpServerConfig> { { "gone", config } });

    manager.connect("gone");
    const auto snapshot = manager.snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot[0].state == imza::McpServerState::FAILED);
    CHECK(snapshot[0].detail.find("no longer exists") != std::string::npos);

    // Calls explain the failure without touching the network.
    imza::McpToolCallResult result;
    std::string detail;
    CHECK(manager.call("gone", "echo", { }, result, detail)
        == imza::Status::CONFIG_ERROR);
}

TEST_CASE("mcp manager resolves catalogue labels and urls from the bundle")
{
    imza::McpServerConfig config;
    config.id         = "docs";
    config.catalog_id = "github";
    imza::McpManager manager(
        std::map<std::string, imza::McpServerConfig> { { "docs", config } });

    // Label falls back to the catalogue entry's label, not the id.
    CHECK(manager.snapshot()[0].label == "GitHub");
}

TEST_CASE("resolved_mcp_url picks the explicit url, the catalogue url, or none")
{
    // A catalogue reference carries no url of its own; the bundled entry's
    // url is what the handshake must use.
    imza::McpServerConfig reference;
    reference.catalog_id = "github";
    const auto entry     = imza::find_mcp_catalog_entry("github");
    REQUIRE(entry.has_value());
    CHECK(imza::resolved_mcp_url(reference) == entry->url);

    // An explicit url wins over the reference.
    imza::McpServerConfig custom;
    custom.catalog_id = "github";
    custom.url        = "http://127.0.0.1:9/custom";
    CHECK(imza::resolved_mcp_url(custom) == "http://127.0.0.1:9/custom");

    // A dangling reference resolves to nothing.
    imza::McpServerConfig dangling;
    dangling.catalog_id = "no-such-entry";
    CHECK(imza::resolved_mcp_url(dangling).empty());
}

namespace {

imza::McpServerConfig stdio_config(const std::string& id)
{
    imza::McpServerConfig config;
    config.id      = id;
    config.type    = "stdio";
    config.command = "imza-no-such-program-xyz";
    return config;
}

} // namespace

TEST_CASE("mcp manager reports a stdio spawn failure as FAILED")
{
    imza::McpManager manager(std::map<std::string, imza::McpServerConfig> {
        { "local", stdio_config("local") } });

    manager.connect("local");
    REQUIRE(wait_state(manager, "local", imza::McpServerState::FAILED));
    CHECK(
        manager.snapshot()[0].detail.find("spawn failed") != std::string::npos);

    imza::McpToolCallResult result;
    std::string detail;
    CHECK(manager.call("local", "echo", { }, result, detail)
        == imza::Status::CONFIG_ERROR);
}

TEST_CASE("mcp manager reload drops a stdio session when its command changes")
{
    imza::McpManager manager(std::map<std::string, imza::McpServerConfig> {
        { "local", stdio_config("local") } });
    // A stdio server is offline until connected; a field change must still
    // reset the entry rather than keep a stale session.
    manager.reload({ { "local", stdio_config("local") } });
    CHECK(manager.snapshot()[0].state == imza::McpServerState::OFFLINE);

    imza::McpServerConfig edited = stdio_config("local");
    edited.args.push_back("--tool-error");
    manager.reload({ { "local", edited } });
    CHECK(manager.snapshot()[0].state == imza::McpServerState::OFFLINE);

    // A presentation-only edit keeps the entry untouched.
    imza::McpServerConfig relabeled = edited;
    relabeled.label                 = "Local";
    manager.reload({ { "local", relabeled } });
    CHECK(manager.snapshot()[0].label == "Local");
    CHECK(manager.snapshot()[0].state == imza::McpServerState::OFFLINE);
}
