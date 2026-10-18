#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "common/util.h"
#include "network/json.h"
#include "runtime/bg_process_subsystem.h"
#include "test_fs.h"
#include "test_helpers.h"
#include "tools/lua.h"
#include "tools/tool.h"

namespace fs = std::filesystem;

namespace {

// Runs a lua script with an explicit timeout through a fresh tool.
imza::ToolOutput run_lua_timeout(
    const std::string& script, long timeout, imza::LuaHost host = { })
{
    auto state            = imza::make_lua_state();
    const imza::Tool tool = imza::make_lua_tool(*state, std::move(host));
    imza::ToolCallRequest req;
    req.name             = "lua";
    imza::JsonValue args = imza::JsonValue::object_t { };
    args.get<imza::JsonValue::object_t>()["script"] = imza::JsonValue(script);
    args.get<imza::JsonValue::object_t>()["timeout"]
        = imza::JsonValue(static_cast<double>(timeout));
    req.args = imza::json_dump(args);
    return imza::dispatch_tool({ &tool, 1 }, req);
}

} // namespace

TEST_CASE("imza.sleep parks in seconds without spending the budget")
{
    // A 1s budget and a 2s park: without deadline credit the next instruction
    // after the sleep would trip the hook. The elapsed floor is what pins the
    // argument down to seconds rather than milliseconds.
    const auto started = std::chrono::steady_clock::now();
    const imza::ToolOutput out
        = run_lua_timeout("imza.sleep(2)\nprint('alive')", 1);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text.find("alive") != std::string::npos);
    CHECK(elapsed >= std::chrono::milliseconds(1900));
}

TEST_CASE("imza.sleep rejects a negative duration")
{
    const imza::ToolOutput out = imza::test::run_lua("imza.sleep(-1)");
    CHECK(out.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(out.text.find("negative") != std::string::npos);
}

TEST_CASE("imza.sleep bails out when the turn is interrupted")
{
    imza::LuaHost host;
    host.interrupted = [] { return true; };
    const imza::ToolOutput out
        = imza::test::run_lua("imza.sleep(60)\nprint('dead')", host);
    CHECK(out.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(out.text.find("interrupted") != std::string::npos);
    CHECK(out.text.find("dead") == std::string::npos);
}

TEST_CASE("the background timeout is clamped to the ceiling")
{
    // Timeouts clamp rather than fail, so the ceiling shows up as the value
    // the host is handed.
    std::chrono::seconds seen_timeout { 0 };
    imza::LuaHost host;
    host.shell_enabled = true;
    host.spawn_bg      = [&](const std::string&, std::chrono::seconds timeout,
                             const std::string&) {
        seen_timeout = timeout;
        return true;
    };
    const imza::ToolOutput out
        = imza::test::run_lua("imza.bg.shell('true', 9999)", host);
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(seen_timeout == std::chrono::seconds(600));
}

TEST_CASE("imza.bg.shell fails closed without a background host")
{
    imza::LuaHost host;
    host.shell_enabled = true;
    const imza::ToolOutput out
        = imza::test::run_lua("imza.bg.shell('true')", host);
    CHECK(out.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(out.text.find("unavailable") != std::string::npos);
}

TEST_CASE("imza.bg.shell rejects chained commands")
{
    imza::LuaHost host;
    host.shell_enabled = true;
    host.spawn_bg      = [](const std::string&, std::chrono::seconds,
                             const std::string&) { return true; };
    const imza::ToolOutput out
        = imza::test::run_lua("imza.bg.shell('true && true')", host);
    CHECK(out.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(out.text.find("one command per call") != std::string::npos);
}

TEST_CASE("imza.bg.shell is gated by the shell capability")
{
    imza::LuaHost host;
    host.shell_enabled = false;
    host.spawn_bg      = [](const std::string&, std::chrono::seconds,
                             const std::string&) { return true; };
    const imza::ToolOutput out
        = imza::test::run_lua("imza.bg.shell('true')", host);
    CHECK(out.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(out.text.find("shell access is disabled") != std::string::npos);
}

TEST_CASE("imza.bg.shell hands the authorized command to the host")
{
    std::string seen_command;
    std::string seen_workspace;
    imza::LuaHost host;
    host.shell_enabled = true;
    host.spawn_bg      = [&](const std::string& command, std::chrono::seconds,
                             const std::string& workspace) {
        seen_command   = command;
        seen_workspace = workspace;
        return true;
    };
    const imza::ToolOutput out
        = imza::test::run_lua("imza.bg.shell('echo hi', 300)", host);
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(seen_command == "echo hi");
}

TEST_CASE("a background process outlives the call that started it")
{
    imza::test::TempDir dir;
    imza::BgProcessSubsystem manager;
    const fs::path marker = dir.path / "done.txt";

    const auto started = std::chrono::steady_clock::now();
    const bool ok      = manager.start("sleep 1; echo hi > " + marker.string(),
        std::chrono::seconds(10), dir.path);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    CHECK(ok);
    // Returns as soon as the process is spawned, not when it finishes.
    CHECK(elapsed < std::chrono::milliseconds(500));
    CHECK(!fs::exists(marker));

    // The process kept running after start() returned.
    for (int i = 0; i < 40 && !fs::exists(marker); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(fs::exists(marker));
}

TEST_CASE("stopping the manager kills a long-running background process")
{
    imza::test::TempDir dir;
    auto manager = std::make_unique<imza::BgProcessSubsystem>();
    REQUIRE(manager->start("sleep 60", std::chrono::seconds(120), dir.path));

    const auto started = std::chrono::steady_clock::now();
    manager.reset();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    // Teardown kills the group instead of waiting out its 120s timeout.
    CHECK(elapsed < std::chrono::seconds(5));
}

TEST_CASE("a background command times out on its own")
{
    imza::test::TempDir dir;
    imza::BgProcessSubsystem manager;
    const fs::path marker = dir.path / "after.txt";

    // The command would write the marker after 5s, but its 1s timeout kills
    // it first, so the marker never appears.
    REQUIRE(manager.start("sleep 5; echo late > " + marker.string(),
        std::chrono::seconds(1), dir.path));
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    CHECK(!fs::exists(marker));
}
