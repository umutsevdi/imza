#include "acp/adapter.h"
#include "acp/protocol.h"
#include "acp/updates.h"
#include "acp/wire.h"
#include "app/flows.h"
#include "conversation/persistence.h"
#include "network/json_io.h"
#include "protocols/mcp.h"
#include "test_fs.h"
#include "test_state.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using imza::JsonValue;

struct WireSink {
    std::mutex mutex;
    std::vector<std::string> lines;

    void write(std::string_view line)
    {
        std::lock_guard lock(mutex);
        lines.emplace_back(line);
    }

    std::vector<std::string> snapshot()
    {
        std::lock_guard lock(mutex);
        return lines;
    }
};

bool contains_line(
    const std::vector<std::string>& lines, const std::string& needle)
{
    for (const std::string& line : lines) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

JsonValue object_of(
    std::initializer_list<std::pair<std::string, JsonValue>> entries)
{
    JsonValue value = JsonValue::object_t { };
    auto& object    = value.get<JsonValue::object_t>();
    for (auto& [key, item] : entries) {
        object[key] = item;
    }
    return value;
}

// Several adapter methods are gated on initialize; tests that drive
// session methods go through this first.
void initialize(imza::acp::AcpInterface& interface)
{
    interface.handle_request(JsonValue(0), "initialize",
        object_of({ { "protocolVersion", JsonValue(1.0) },
            { "clientInfo", object_of({ { "name", JsonValue("test") } }) } }));
}

// Persists one finished one-round conversation for `client` and returns
// its session id; the fixture behind the load/list/lock tests.
std::string save_finished_session(
    imza::PostFn post, const std::string& client, const std::string& text)
{
    auto state = imza::test::make_test_state(std::move(post));
    imza::SessionSnapshot snapshot;
    snapshot.client = client;
    state->session->restore(std::move(snapshot));
    state->session->begin_send("hello");
    state->session->append_assistant("m", "default");
    state->session->apply(imza::make_delta_event(text), imza::ModelPricing { });
    state->session->apply(imza::make_done_event(), imza::ModelPricing { });
    state->session->finish_session("");
    REQUIRE(imza::save_session(*state->session) == imza::Status::OK);
    return state->session->session_id();
}

std::string created_session_id(const std::vector<std::string>& lines)
{
    constexpr std::string_view marker = "\"sessionId\":\"";
    for (const std::string& line : lines) {
        const std::size_t at = line.find(marker);
        if (at == std::string::npos) {
            continue;
        }
        const std::size_t start = at + marker.size();
        const std::size_t end   = line.find('"', start);
        return line.substr(start, end - start);
    }
    return { };
}
} // namespace

TEST_CASE("unknown methods return -32601; extension requests too")
{
    imza::test::PostPump pump;
    imza::test::IsolatedDataHome data_home;
    auto state = imza::test::make_test_state(pump.fn());
    WireSink sink;
    imza::acp::AcpInterface interface(*state, { });
    interface.set_writer([&sink](std::string_view line) { sink.write(line); });
    initialize(interface);

    interface.handle_request(
        JsonValue(1), "totally/bogus", JsonValue::object_t { });
    interface.handle_request(
        JsonValue(2), "_com.example/thing", JsonValue::object_t { });

    const auto lines = sink.snapshot();
    CHECK(contains_line(lines, "\"error\""));
    CHECK(contains_line(lines, "-32601"));
}

TEST_CASE("session/new registers a session; unknown prompt session 404s")
{
    imza::test::PostPump pump;
    imza::test::IsolatedDataHome data_home;
    auto state = imza::test::make_test_state(pump.fn());
    WireSink sink;
    imza::acp::AcpInterface interface(*state, { });
    interface.set_writer([&sink](std::string_view line) { sink.write(line); });
    initialize(interface);

    interface.handle_request(
        JsonValue(1), "session/new", JsonValue::object_t { });
    auto lines = sink.snapshot();
    CHECK(contains_line(lines, "\"sessionId\":\""));
    CHECK(contains_line(lines, "\"configOptions\""));
    CHECK(contains_line(lines, "\"category\":\"mode\""));
    CHECK(contains_line(lines, "\"availableCommands\""));
    // The protocol id is imza's own session id, never a synthetic
    // counter: it is the stem the session saves under, so a client that
    // remembers it can load the conversation back after a restart.
    const std::string created = created_session_id(lines);
    REQUIRE_FALSE(created.empty());
    CHECK(created.find("imza-") != 0);

    interface.handle_request(JsonValue(2), "session/prompt",
        object_of({ { "sessionId", JsonValue("missing") } }));
    lines = sink.snapshot();
    CHECK(contains_line(lines, "-32002"));
}

TEST_CASE("execution label matches the UI card label")
{
    imza::ToolCall call;
    call.name  = "lua";
    call.phase = imza::ToolCall::Phase::EXECUTING;
    CHECK(imza::acp::execution_label(call) == "Executing");

    call.result = imza::ToolCall::Result { imza::ToolCall::Result::Kind::OUTPUT,
        "done" };
    CHECK(imza::acp::execution_label(call) == "Executed");

    call.result->dispatch_log = {
        { "fs.read", "/a", true },
        { "fs.read", "/b", true },
        { "shell", "ls", true },
    };
    CHECK(imza::acp::execution_label(call)
        == "Executed \xC2\xB7 2 reads \xC2\xB7 1 command");

    call.result->kind = imza::ToolCall::Result::Kind::ERROR;
    CHECK(imza::acp::execution_label(call) == "Execution Failed");
}

TEST_CASE("permission prompts map to request_permission options")
{
    imza::PermissionPrompt prompt;
    prompt.name              = "shell";
    prompt.description       = "run ls";
    prompt.allow_for_session = true;
    prompt.id                = "call-9";

    const auto request = imza::acp::permission_request_for("s", prompt);
    REQUIRE(request.has_value());
    CHECK(request->tool_call_id == "call-9");
    REQUIRE(request->options.size() == 4);
    CHECK(request->options[0].kind == "allow_once");
    CHECK(request->options[1].kind == "allow_always");
    CHECK(request->options[2].kind == "reject_always");
    CHECK(request->options[3].kind == "reject_once");

    prompt.allow_for_session = false;
    CHECK(imza::acp::permission_request_for("s", prompt)->options.size() == 2);

    const JsonValue outcome = object_of({ { "outcome",
        object_of({ { "outcome", JsonValue("selected") },
            { "optionId", JsonValue("allow-session") } }) } });
    CHECK(imza::acp::permission_outcome_kind(outcome) == "allow_always");
    CHECK(imza::acp::permission_outcome_kind(
        object_of({ { "outcome",
            object_of({ { "outcome", JsonValue("cancelled") } }) } }))
            .empty());
}

TEST_CASE("prompt turn runs end-to-end and persists an ACP session")
{
    imza::test::IsolatedDataHome data_home;
    auto state = imza::test::make_test_state(imza::PostFn { },
        imza::test::test_config(),
        [](const imza::ChatRequest&, const imza::StreamCallback& cb) {
            cb(imza::make_connected_event());
            cb(imza::make_delta_event("hello"));
            cb(imza::make_done_event());
            return imza::Status::OK;
        });
    WireSink sink;
    imza::acp::AcpInterface interface(*state, { });
    interface.set_writer([&sink](std::string_view line) { sink.write(line); });
    initialize(interface);

    imza::test::wait_for_environment(*state);
    interface.handle_request(
        JsonValue(1), "session/new", JsonValue::object_t { });
    const auto lines             = sink.snapshot();
    const std::string session_id = created_session_id(sink.snapshot());
    REQUIRE_FALSE(session_id.empty());

    interface.handle_request(JsonValue(2), "session/prompt",
        object_of({ { "sessionId", JsonValue(session_id) },
            { "prompt",
                JsonValue::array_t { object_of({ { "type", JsonValue("text") },
                    { "text", JsonValue("say hi") } }) } } }));
    CHECK(imza::test::wait_until(
        [&] { return contains_line(sink.snapshot(), "\"stopReason\""); }));

    const auto after = sink.snapshot();
    CHECK(contains_line(after, R"("stopReason":"end_turn")"));
    CHECK(contains_line(after, "\"agent_message_chunk\""));

    // Sessions made through ACP are tagged with the client; the helper
    // that selects them is saved_sessions_of with a non-empty client.
    const std::vector<imza::SavedSession> saved
        = imza::saved_sessions_of("test");
    CHECK_FALSE(saved.empty());
    for (const imza::SavedSession& entry : saved) {
        CHECK_FALSE(entry.client.empty());
        CHECK(entry.workspace.has_value());
    }
}

TEST_CASE("cancellation answers stopReason cancelled")
{
    std::atomic<bool> gate { false };
    std::atomic<bool> turn_started { false };
    imza::test::IsolatedDataHome data_home;
    auto state = imza::test::make_test_state(imza::PostFn { },
        imza::test::test_config(),
        [&](const imza::ChatRequest&, const imza::StreamCallback& cb) {
            cb(imza::make_connected_event());
            cb(imza::make_delta_event("partial"));
            turn_started.store(true);
            // Hold the turn open until the test cancels, then finish;
            // the runner's post-stream interrupt check sees the flag and
            // reports the cancellation.
            imza::test::wait_until([&] { return gate.load(); });
            cb(imza::make_done_event());
            return imza::Status::OK;
        });
    WireSink sink;
    imza::acp::AcpInterface interface(*state, { });
    interface.set_writer([&sink](std::string_view line) { sink.write(line); });
    initialize(interface);

    imza::test::wait_for_environment(*state);
    interface.handle_request(
        JsonValue(1), "session/new", JsonValue::object_t { });
    std::string session_id;
    for (const std::string& line : sink.snapshot()) {
        constexpr std::string_view marker = "\"sessionId\":\"";
        const std::size_t at              = line.find(marker);
        if (at != std::string::npos) {
            const std::size_t start = at + marker.size();
            const std::size_t end   = line.find('"', start);
            session_id              = line.substr(start, end - start);
        }
    }
    REQUIRE_FALSE(session_id.empty());
    interface.handle_request(JsonValue(2), "session/prompt",
        object_of({ { "sessionId", JsonValue(session_id) },
            { "prompt",
                JsonValue::array_t { object_of({ { "type", JsonValue("text") },
                    { "text", JsonValue("long story") } }) } } }));
    // Wait for the turn worker to sit in the stub, then cancel; the
    // notification must reach the agent while the prompt is in flight.
    CHECK(imza::test::wait_until([&] { return turn_started.load(); }));
    interface.handle_notification("session/cancel",
        object_of({ { "sessionId", JsonValue(session_id) } }));
    gate.store(true);

    CHECK(imza::test::wait_until([&] {
        return contains_line(sink.snapshot(), R"("stopReason":"cancelled")");
    }));
}

TEST_CASE("tool lifecycle frames: canonical call then labeled result update")
{
    imza::test::IsolatedDataHome data_home;
    auto state = imza::test::make_test_state(imza::PostFn { },
        imza::test::test_config(),
        [called = 0](
            const imza::ChatRequest&, const imza::StreamCallback& cb) mutable {
            // Tool call in the first stream; later rounds answer with
            // text only, like a model that got its result.
            if (called++ == 0) {
                cb(imza::make_connected_event());
                cb(imza::make_tool_call_start_event(
                    { "lua", "{}", "", "tc-1" }));
                cb(imza::make_tool_call_event(
                    { "lua", R"({"script":"return 1+1"})", "", "tc-1" }));
                cb(imza::make_done_event());
            } else {
                cb(imza::make_delta_event("2"));
                cb(imza::make_done_event());
            }
            return imza::Status::OK;
        });
    WireSink sink;
    imza::acp::AcpInterface interface(*state, { });
    interface.set_writer([&sink](std::string_view line) { sink.write(line); });
    initialize(interface);

    imza::test::wait_for_environment(*state);
    interface.handle_request(
        JsonValue(1), "session/new", JsonValue::object_t { });
    const std::string session_id = created_session_id(sink.snapshot());
    REQUIRE_FALSE(session_id.empty());

    interface.handle_request(JsonValue(2), "session/prompt",
        object_of({ { "sessionId", JsonValue(session_id) },
            { "prompt",
                JsonValue::array_t { object_of({ { "type", JsonValue("text") },
                    { "text", JsonValue("run 1+1") } }) } } }));
    CHECK(imza::test::wait_until(
        [&] { return contains_line(sink.snapshot(), "\"stopReason\""); }));

    const auto lines = sink.snapshot();
    CHECK(contains_line(lines, "\"sessionUpdate\":\"tool_call\""));
    CHECK(contains_line(lines, R"("title":"Executing")"));
    CHECK(contains_line(lines, "\"sessionUpdate\":\"tool_call_update\""));
    CHECK(contains_line(lines, R"("title":"Executed")"));
    CHECK(contains_line(lines, R"("toolCallId":"tc-1")"));
}

TEST_CASE("tool output renders as a report block, not raw fields")
{
    imza::ToolCall call;
    call.call_id = "tc-9";
    call.name    = "lua";
    call.args    = R"js({"script":"imza.fs.read('a.md')"})js";
    call.phase   = imza::ToolCall::Phase::EXECUTING;
    call.result = imza::ToolCall::Result { imza::ToolCall::Result::Kind::OUTPUT,
        "# Heading\n\n- item" };

    imza::acp::UpdateBuilder builder("sess");
    const std::string frame = builder.tool_call_update(call);

    CHECK(frame.find("\"type\":\"content\"") != std::string::npos);
    CHECK(frame.find("```lua") != std::string::npos);
    CHECK(frame.find("# Heading") != std::string::npos);
    CHECK(frame.find("\"rawInput\"") == std::string::npos);
    CHECK(frame.find("\"rawOutput\"") == std::string::npos);
}

TEST_CASE("session/list scopes to the client and reports cwd and updatedAt")
{
    imza::test::PostPump pump;
    imza::test::IsolatedDataHome data_home;
    auto state = imza::test::make_test_state(pump.fn());
    WireSink sink;
    imza::acp::AcpInterface zed(*state, { });
    zed.set_writer([&sink](std::string_view line) { sink.write(line); });
    zed.handle_request(JsonValue(1), "initialize",
        object_of({ { "protocolVersion", JsonValue(1.0) },
            { "clientInfo", object_of({ { "name", JsonValue("zed") } }) } }));
    zed.handle_request(JsonValue(2), "session/new", JsonValue::object_t { });
    const std::string zed_session = created_session_id(sink.snapshot());
    REQUIRE_FALSE(zed_session.empty());

    // A second connection, a different client, a different directory.
    imza::test::TempDir elsewhere;
    const imza::test::CurrentDirectory origin;
    std::filesystem::current_path(elsewhere.path);
    WireSink other_sink;
    imza::acp::AcpInterface imzabot(*state, { });
    imzabot.set_writer(
        [&other_sink](std::string_view line) { other_sink.write(line); });
    imzabot.handle_request(JsonValue(1), "initialize",
        object_of({ { "protocolVersion", JsonValue(1.0) },
            { "clientInfo",
                object_of({ { "name", JsonValue("imzabot") } }) } }));
    // A saved session is what the listing reports on: an unsaved one has
    // no index entry and therefore no cwd or updatedAt.
    const std::string bot_session
        = save_finished_session(pump.fn(), "imzabot", "hi");
    REQUIRE_FALSE(bot_session.empty());

    imzabot.handle_request(
        JsonValue(3), "session/list", JsonValue::object_t { });
    const auto bot_lines = other_sink.snapshot();
    CHECK(contains_line(bot_lines, bot_session));
    CHECK_FALSE(contains_line(bot_lines, zed_session));
    // The listing carries an absolute cwd and an RFC 3339 stamp.
    bool saw_stamp = false;
    for (const std::string& line : bot_lines) {
        CHECK(line.find("\"cwd\":\"\"") == std::string::npos);
        if (line.find("T") != std::string::npos
            && line.find("Z\"") != std::string::npos) {
            saw_stamp = true;
        }
    }
    CHECK(contains_line(bot_lines,
        "\"cwd\":\"" + imza::portable_string(elsewhere.path) + "\""));
    CHECK(saw_stamp);

    // Another client cannot load this one's session.
    imzabot.handle_request(JsonValue(4), "session/load",
        object_of({ { "sessionId", JsonValue(zed_session) } }));
    CHECK(contains_line(other_sink.snapshot(), "-32002"));

    // A relative cwd filter is rejected rather than silently ignored.
    imzabot.handle_request(JsonValue(5), "session/list",
        object_of({ { "cwd", JsonValue("relative/path") } }));
    CHECK(contains_line(other_sink.snapshot(), "-32602"));
}

TEST_CASE("mode switches through both client methods and announces itself")
{
    imza::test::PostPump pump;
    imza::test::IsolatedDataHome data_home;
    auto state = imza::test::make_test_state(pump.fn());
    WireSink sink;
    imza::acp::AcpInterface interface(*state, { });
    interface.set_writer([&sink](std::string_view line) { sink.write(line); });
    initialize(interface);
    interface.handle_request(
        JsonValue(1), "session/new", JsonValue::object_t { });
    const std::string id = created_session_id(sink.snapshot());
    REQUIRE_FALSE(id.empty());
    // Both switcher surfaces ride every session reply.
    CHECK(contains_line(sink.snapshot(), "\"configOptions\""));
    CHECK(contains_line(sink.snapshot(), R"("currentModeId":"plan")"));
    CHECK(contains_line(sink.snapshot(), "\"availableModes\""));

    // The legacy method names the target modeId, not configId.
    interface.handle_request(JsonValue(2), "session/set_mode",
        object_of({ { "sessionId", JsonValue(id) },
            { "modeId", JsonValue("build") } }));
    CHECK(contains_line(sink.snapshot(), "\"currentModeId\":\"build\""));
    CHECK(contains_line(sink.snapshot(), R"("currentValue":"build")"));
    // The switch is announced, so a client that only follows
    // notifications sees it.
    CHECK(contains_line(
        sink.snapshot(), "\"sessionUpdate\":\"current_mode_update\""));

    interface.handle_request(JsonValue(3), "session/set_config_option",
        object_of(
            { { "sessionId", JsonValue(id) }, { "configId", JsonValue("mode") },
                { "value", JsonValue("plan") } }));
    CHECK(contains_line(sink.snapshot(), "\"currentModeId\":\"plan\""));

    // A switch made from the session side, with no client request
    // involved, still reaches the client as a notification.
    interface.handle_request(JsonValue(4), "session/prompt",
        object_of({ { "sessionId", JsonValue(id) },
            { "prompt",
                JsonValue::array_t {
                    JsonValue(object_of({ { "type", JsonValue("text") },
                        { "text", JsonValue("/mode build") } })) } } }));
    CHECK(imza::test::wait_until([&] {
        return contains_line(sink.snapshot(), R"("currentModeId":"build")");
    }));
}

TEST_CASE("a cwd other than the process directory is rejected")
{
    imza::test::PostPump pump;
    imza::test::IsolatedDataHome data_home;
    auto state = imza::test::make_test_state(pump.fn());
    WireSink sink;
    imza::acp::AcpInterface interface(*state, { });
    interface.set_writer([&sink](std::string_view line) { sink.write(line); });
    initialize(interface);

    interface.handle_request(JsonValue(1), "session/new",
        object_of({ { "cwd", JsonValue("/nonexistent-imza-test-dir") } }));
    CHECK(contains_line(sink.snapshot(), "-32602"));
    CHECK(contains_line(sink.snapshot(), "cwd must be the directory"));

    interface.handle_request(JsonValue(2), "session/new",
        object_of({ { "cwd",
            JsonValue(
                imza::portable_string(std::filesystem::current_path())) } }));
    CHECK(contains_line(sink.snapshot(), "\"sessionId\":\""));
}

TEST_CASE("session/load restores a saved ACP session without crashing")
{
    imza::test::PostPump pump;
    imza::test::IsolatedDataHome data_home;
    auto state = imza::test::make_test_state(pump.fn());
    WireSink sink;
    imza::acp::AcpInterface interface(*state, { });
    interface.set_writer([&sink](std::string_view line) { sink.write(line); });
    // Loads are scoped to the client that created the session.
    interface.handle_request(JsonValue(0), "initialize",
        object_of({ { "protocolVersion", JsonValue(1.0) },
            { "clientInfo", object_of({ { "name", JsonValue("zed") } }) } }));

    const std::string id = save_finished_session(pump.fn(), "zed", "hi there");

    interface.handle_request(JsonValue(1), "session/load",
        object_of({ { "sessionId", JsonValue(id) } }));
    const auto lines = sink.snapshot();
    CHECK(contains_line(lines, "\"result\""));
    CHECK(contains_line(lines, "user_message_chunk"));
    CHECK(contains_line(lines, "agent_message_chunk"));
    CHECK_FALSE(contains_line(lines, "-32002"));
}
TEST_CASE("a loaded session is locked; a second load is refused until closed")
{
    imza::test::PostPump pump;
    imza::test::IsolatedDataHome data_home;
    auto state = imza::test::make_test_state(pump.fn());

    const std::string id = save_finished_session(pump.fn(), "zed", "hi there");
    const auto session_path = imza::sessions_dir() / (id + ".json");

    imza::acp::AcpInterface first(*state, { });
    WireSink first_sink;
    first.set_writer(
        [&first_sink](std::string_view line) { first_sink.write(line); });
    first.handle_request(JsonValue(0), "initialize",
        object_of({ { "protocolVersion", JsonValue(1.0) },
            { "clientInfo", object_of({ { "name", JsonValue("zed") } }) } }));

    first.handle_request(JsonValue(1), "session/load",
        object_of({ { "sessionId", JsonValue(id) } }));
    CHECK(contains_line(first_sink.snapshot(), "\"result\""));
    CHECK(imza::session_file_locked(session_path));

    // A second connection (another editor window) must be refused instead
    // of opening a duplicate writer on the same file.
    imza::acp::AcpInterface second(*state, { });
    WireSink second_sink;
    second.set_writer(
        [&second_sink](std::string_view line) { second_sink.write(line); });
    second.handle_request(JsonValue(0), "initialize",
        object_of({ { "protocolVersion", JsonValue(1.0) },
            { "clientInfo", object_of({ { "name", JsonValue("zed") } }) } }));
    second.handle_request(JsonValue(1), "session/load",
        object_of({ { "sessionId", JsonValue(id) } }));
    CHECK(contains_line(second_sink.snapshot(), "-32002"));
    CHECK(contains_line(second_sink.snapshot(), "open in another"));

    // The first connection still works: the refusal did not disturb it.
    first.handle_request(JsonValue(2), "session/list", JsonValue::object_t { });
    CHECK(contains_line(first_sink.snapshot(), id));

    first.handle_request(JsonValue(3), "session/close",
        object_of({ { "sessionId", JsonValue(id) } }));
    CHECK_FALSE(imza::session_file_locked(session_path));

    // The lock is gone, so the second connection can take over.
    second.handle_request(JsonValue(2), "session/load",
        object_of({ { "sessionId", JsonValue(id) } }));
    CHECK(contains_line(second_sink.snapshot(), "\"result\""));
    CHECK(imza::session_file_locked(session_path));
}

TEST_CASE("a TUI-held session lock refuses the ACP load")
{
    imza::test::PostPump pump;
    imza::test::IsolatedDataHome data_home;
    auto state = imza::test::make_test_state(pump.fn());

    const std::string id = save_finished_session(pump.fn(), "zed", "hi there");
    const auto session_path = imza::sessions_dir() / (id + ".json");

    imza::test::ForeignLock tui(session_path);

    imza::acp::AcpInterface interface(*state, { });
    WireSink sink;
    interface.set_writer([&sink](std::string_view line) { sink.write(line); });
    interface.handle_request(JsonValue(0), "initialize",
        object_of({ { "protocolVersion", JsonValue(1.0) },
            { "clientInfo", object_of({ { "name", JsonValue("zed") } }) } }));
    interface.handle_request(JsonValue(1), "session/load",
        object_of({ { "sessionId", JsonValue(id) } }));
    CHECK(contains_line(sink.snapshot(), "-32002"));
}

TEST_CASE("delete propagates the lock result instead of always succeeding")
{
    imza::test::PostPump pump;
    imza::test::IsolatedDataHome data_home;
    auto state = imza::test::make_test_state(pump.fn());

    const std::string id = save_finished_session(pump.fn(), "zed", "hi there");
    const auto session_path = imza::sessions_dir() / (id + ".json");

    imza::acp::AcpInterface interface(*state, { });
    WireSink sink;
    interface.set_writer([&sink](std::string_view line) { sink.write(line); });
    interface.handle_request(JsonValue(0), "initialize",
        object_of({ { "protocolVersion", JsonValue(1.0) },
            { "clientInfo", object_of({ { "name", JsonValue("zed") } }) } }));

    // A foreign TUI holder keeps the file alive; the reply is an error.
    {
        imza::test::ForeignLock tui(session_path);
        interface.handle_request(JsonValue(1), "session/delete",
            object_of({ { "sessionId", JsonValue(id) } }));
        CHECK(contains_line(sink.snapshot(), "\"error\""));
        CHECK(std::filesystem::exists(session_path));
    }
    // Unlocked, the delete succeeds and the file is gone.
    interface.handle_request(JsonValue(2), "session/delete",
        object_of({ { "sessionId", JsonValue(id) } }));
    CHECK(contains_line(sink.snapshot(), "\"result\""));
    CHECK_FALSE(std::filesystem::exists(session_path));

    // Deleting an unknown or already-deleted id succeeds silently; the
    // spec says clients should never see an error here.
    interface.handle_request(JsonValue(3), "session/delete",
        object_of({ { "sessionId", JsonValue("nope") } }));
    CHECK(contains_line(sink.snapshot(), "\"result\""));
}

const JsonValue* member(const JsonValue& value, const std::string& key)
{
    return imza::find_member(value, key);
}

TEST_CASE("initialize negotiates the protocol version and capabilities")
{
    // A supported version is echoed; an unsupported one gets the latest
    // imza speaks, so a matching client stays and a mismatched one quits.
    const imza::acp::InitializeRequest supported
        = imza::acp::InitializeRequest::parse(
            object_of({ { "protocolVersion", JsonValue(1.0) } }));
    const JsonValue echoed = imza::acp::dump_initialize_result(supported);
    CHECK(member(echoed, "protocolVersion")->get<double>()
        == doctest::Approx(1.0));
    const imza::acp::InitializeRequest future
        = imza::acp::InitializeRequest::parse(
            object_of({ { "protocolVersion", JsonValue(99.0) } }));
    const JsonValue downgraded = imza::acp::dump_initialize_result(future);
    CHECK(member(downgraded, "protocolVersion")->get<double>()
        == doctest::Approx(1.0));

    // Client capability advertisements the adapter gates behavior on.
    JsonValue caps            = JsonValue(JsonValue::object_t { });
    JsonValue fs              = JsonValue(JsonValue::object_t { });
    fs["readTextFile"]        = true;
    caps["fs"]                = fs;
    JsonValue elicitation     = JsonValue(JsonValue::object_t { });
    elicitation["form"]       = JsonValue(JsonValue::object_t { });
    elicitation["url"]        = JsonValue { };
    caps["elicitation"]       = elicitation;
    JsonValue session         = JsonValue(JsonValue::object_t { });
    session["compaction"]     = JsonValue(JsonValue::object_t { });
    session["notices"]        = JsonValue(JsonValue::object_t { });
    JsonValue config_options  = JsonValue(JsonValue::object_t { });
    config_options["boolean"] = JsonValue(JsonValue::object_t { });
    session["configOptions"]  = config_options;
    caps["session"]           = session;
    const imza::acp::ClientCapabilities parsed
        = imza::acp::ClientCapabilities::parse(caps);
    CHECK(parsed.elicitation_form);
    CHECK(parsed.compaction);
    CHECK(parsed.notices);
    CHECK(parsed.boolean_options);

    // The agent result advertises what imza actually supports.
    const JsonValue* agent = member(echoed, "agentCapabilities");
    REQUIRE(agent != nullptr);
    const JsonValue* prompt = member(*agent, "promptCapabilities");
    REQUIRE(prompt != nullptr);
    CHECK(member(*prompt, "image")->get<bool>());
    CHECK(member(*prompt, "embeddedContext")->get<bool>());
    // Every implemented session method must be advertised, or a conforming
    // client is forbidden from calling it.
    const JsonValue* sessions = member(*agent, "sessionCapabilities");
    REQUIRE(sessions != nullptr);
    for (const char* method :
        { "list", "delete", "resume", "close", "additionalDirectories" }) {
        CHECK(member(*sessions, method) != nullptr);
    }
    CHECK(member(*agent, "loadSession")->get<bool>());
    // No authMethods: provider sign-in stays in the TUI; advertising a
    // terminal method makes clients relaunch for interactive login.
    CHECK(member(echoed, "authMethods") == nullptr);
    const JsonValue* mcp = member(*agent, "mcpCapabilities");
    REQUIRE(mcp != nullptr);
    CHECK(member(*mcp, "http")->get<bool>());
}

TEST_CASE("requests before initialize answer -32600")
{
    imza::test::PostPump pump;
    imza::test::IsolatedDataHome data_home;
    auto state = imza::test::make_test_state(pump.fn());
    WireSink sink;
    imza::acp::AcpInterface interface(*state, { });
    interface.set_writer([&sink](std::string_view line) { sink.write(line); });

    interface.handle_request(
        JsonValue(1), "session/list", JsonValue::object_t { });
    CHECK(contains_line(sink.snapshot(), "-32600"));

    // After initialize the same request is fine.
    initialize(interface);
    interface.handle_request(
        JsonValue(2), "session/list", JsonValue::object_t { });
    CHECK(contains_line(sink.snapshot(), "\"sessions\""));
}

TEST_CASE("resource blocks become attachments or text")
{
    using Kind              = imza::acp::PromptContent::Kind;
    JsonValue text_resource = object_of({ { "type", JsonValue("resource") },
        { "resource",
            object_of({ { "uri", JsonValue("file:///tmp/notes.txt") },
                { "text", JsonValue("the notes") } }) } });
    JsonValue link = object_of({ { "type", JsonValue("resource_link") },
        { "uri", JsonValue("file:///tmp/data.csv") },
        { "name", JsonValue("data.csv") } });
    JsonValue blob_resource = object_of({ { "type", JsonValue("resource") },
        { "resource",
            object_of(
                { { "uri", JsonValue("x") }, { "blob", JsonValue("AAAA") },
                    { "mimeType", JsonValue("image/png") } }) } });
    JsonValue params        = object_of({ { "sessionId", JsonValue("s") } });
    params["prompt"]
        = JsonValue(JsonValue::array_t { text_resource, link, blob_resource });
    const imza::acp::PromptRequest request
        = imza::acp::PromptRequest::parse(params);
    REQUIRE(request.prompt.size() == 3);
    CHECK(request.prompt[0].kind == Kind::RESOURCE);
    CHECK(request.prompt[0].uri == "file:///tmp/notes.txt");
    CHECK(request.prompt[0].text == "the notes");
    CHECK(request.prompt[1].kind == Kind::RESOURCE_LINK);
    CHECK(request.prompt[1].name == "data.csv");
    CHECK(request.prompt[2].kind == Kind::RESOURCE);
    CHECK(request.prompt[2].data == "AAAA");
}

TEST_CASE("config options are typed objects with values")
{
    const std::vector<imza::acp::ConfigOption> options
        = imza::acp::config_options("plan", "gpt-5", "default",
            { { "gpt-5", "GPT-5" }, { "claude-4", "Claude 4" } }, true);
    const JsonValue dumped = imza::acp::dump_config_options(options);
    REQUIRE(dumped.is_array());
    const auto& entries = dumped.get<JsonValue::array_t>();
    REQUIRE(entries.size() == 4);

    // Select options carry {value,name} objects, never bare strings.
    const JsonValue& mode = entries[0];
    CHECK(member(mode, "currentValue")->get<std::string>() == "plan");
    const auto& mode_values
        = member(mode, "options")->get<JsonValue::array_t>();
    REQUIRE(mode_values.size() == 2);
    CHECK(mode_values[0].at("value").get<std::string>() == "plan");
    CHECK(mode_values[0].at("name").get<std::string>() == "Plan");

    // The model catalog is exposed when a connection is active.
    const JsonValue& model = entries[1];
    const auto& models = member(model, "options")->get<JsonValue::array_t>();
    REQUIRE(models.size() == 2);
    CHECK(models[1].at("value").get<std::string>() == "claude-4");

    // The boolean option carries a boolean currentValue and no options.
    const JsonValue& skip = entries[3];
    CHECK(member(skip, "type")->get<std::string>() == "boolean");
    CHECK(member(skip, "currentValue")->get<bool>());
    CHECK(member(skip, "options") == nullptr);
}

TEST_CASE("available commands carry an input hint")
{
    const JsonValue entry = imza::acp::available_command(
        "variant", "Set reasoning effort", "one of: off, low");
    CHECK(member(entry, "input")->at("hint").get<std::string>()
        == "one of: off, low");
    const JsonValue plain
        = imza::acp::available_command("new", "Start a new session");
    CHECK(member(plain, "input") == nullptr);
}

TEST_CASE("todo changes push plan updates to the client")
{
    imza::test::IsolatedDataHome data_home;
    // The turn runs one lua call that sets the todo; the observer on the
    // child session must push it as a plan update, not only on replay.
    int rounds = 0;
    auto state = imza::test::make_test_state(imza::PostFn { },
        imza::test::test_config(),
        [&](const imza::ChatRequest&, const imza::StreamCallback& cb) {
            if (++rounds == 1) {
                cb(imza::make_connected_event());
                cb(imza::make_tool_call_event({ "lua",
                    R"json({"script":"imza.todo.set({{content='research', status='in_progress'}})"})json",
                    "", "tc-1" }));
            } else {
                cb(imza::make_delta_event("planned"));
            }
            cb(imza::make_done_event());
            return imza::Status::OK;
        });
    WireSink sink;
    imza::acp::AcpInterface interface(*state, { });
    interface.set_writer([&sink](std::string_view line) { sink.write(line); });
    initialize(interface);
    interface.handle_request(
        JsonValue(1), "session/new", JsonValue::object_t { });
    const std::string id = created_session_id(sink.snapshot());
    REQUIRE_FALSE(id.empty());
    imza::test::wait_for_environment(*state);

    interface.handle_request(JsonValue(2), "session/prompt",
        object_of({ { "sessionId", JsonValue(id) },
            { "prompt",
                JsonValue::array_t { object_of({ { "type", JsonValue("text") },
                    { "text", JsonValue("plan it") } }) } } }));
    CHECK(imza::test::wait_until([&] {
        return contains_line(sink.snapshot(), R"("sessionUpdate":"plan")");
    }));
    CHECK(contains_line(sink.snapshot(), "\"research\""));
}

TEST_CASE("usage changes push usage_update to the client")
{
    imza::test::IsolatedDataHome data_home;
    auto state = imza::test::make_test_state(imza::PostFn { },
        imza::test::test_config(),
        [](const imza::ChatRequest&, const imza::StreamCallback& cb) {
            cb(imza::make_connected_event());
            cb(imza::make_delta_event("hi"));
            cb(imza::make_usage_event({ 10, 5, 0, 0, 15 }));
            cb(imza::make_done_event());
            return imza::Status::OK;
        });
    WireSink sink;
    imza::acp::AcpInterface interface(*state, { });
    interface.set_writer([&sink](std::string_view line) { sink.write(line); });
    initialize(interface);
    interface.handle_request(
        JsonValue(1), "session/new", JsonValue::object_t { });
    const std::string id = created_session_id(sink.snapshot());
    REQUIRE_FALSE(id.empty());
    imza::test::wait_for_environment(*state);

    interface.handle_request(JsonValue(2), "session/prompt",
        object_of({ { "sessionId", JsonValue(id) },
            { "prompt",
                JsonValue::array_t { object_of({ { "type", JsonValue("text") },
                    { "text", JsonValue("say hi") } }) } } }));
    CHECK(imza::test::wait_until([&] {
        return contains_line(
            sink.snapshot(), R"("sessionUpdate":"usage_update")");
    }));
    CHECK(contains_line(sink.snapshot(), R"("used":15)"));
}

TEST_CASE("an unknown config option answers -32602")
{
    imza::test::PostPump pump;
    imza::test::IsolatedDataHome data_home;
    auto state = imza::test::make_test_state(pump.fn());
    WireSink sink;
    imza::acp::AcpInterface interface(*state, { });
    interface.set_writer([&sink](std::string_view line) { sink.write(line); });
    initialize(interface);
    interface.handle_request(
        JsonValue(1), "session/new", JsonValue::object_t { });
    const std::string id = created_session_id(sink.snapshot());
    REQUIRE_FALSE(id.empty());

    interface.handle_request(JsonValue(2), "session/set_config_option",
        object_of({ { "sessionId", JsonValue(id) },
            { "configId", JsonValue("nonsense") },
            { "value", JsonValue("x") } }));
    CHECK(contains_line(sink.snapshot(), "-32602"));

    // An unknown effort value is rejected too.
    interface.handle_request(JsonValue(3), "session/set_config_option",
        object_of({ { "sessionId", JsonValue(id) },
            { "configId", JsonValue("thought_level") },
            { "value", JsonValue("ultra") } }));
    CHECK(contains_line(sink.snapshot(), "-32602"));
}

TEST_CASE("additionalDirectories widen the session's grants")
{
    imza::test::TempDir extra;
    imza::test::PostPump pump;
    imza::test::IsolatedDataHome data_home;
    auto state = imza::test::make_test_state(pump.fn());
    WireSink sink;
    imza::acp::AcpInterface interface(*state, { });
    interface.set_writer([&sink](std::string_view line) { sink.write(line); });
    initialize(interface);

    interface.handle_request(JsonValue(1), "session/new",
        object_of({ { "additionalDirectories",
            JsonValue(JsonValue::array_t {
                JsonValue(imza::portable_string(extra.path)) }) } }));
    const std::string id = created_session_id(sink.snapshot());
    REQUIRE_FALSE(id.empty());
    // Each extra root installs as a session grant.
    imza::PermissionStore::Snapshot grants = state->permissions->snapshot();
    REQUIRE(grants != nullptr);
    bool found = false;
    for (const imza::PermissionGrant& grant : *grants) {
        if (const auto* dir = std::get_if<imza::ExternalGrant>(&grant)) {
            if (imza::portable_string(*dir)
                == imza::portable_string(extra.path)) {
                found = true;
            }
        }
    }
    CHECK(found);
}

TEST_CASE("streamed chunks carry message ids that change at boundaries")
{
    imza::test::IsolatedDataHome data_home;
    int rounds = 0;
    auto state = imza::test::make_test_state(imza::PostFn { },
        imza::test::test_config(),
        [&](const imza::ChatRequest&, const imza::StreamCallback& cb) {
            ++rounds;
            if (rounds == 1) {
                cb(imza::make_connected_event());
                cb(imza::make_reasoning_event("thinking"));
                cb(imza::make_delta_event("one"));
                cb(imza::make_tool_call_event({ "lua", "{}", "", "tc-1" }));
            } else {
                cb(imza::make_delta_event("two"));
            }
            cb(imza::make_done_event());
            return imza::Status::OK;
        });
    WireSink sink;
    imza::acp::AcpInterface interface(*state, { });
    interface.set_writer([&sink](std::string_view line) { sink.write(line); });
    initialize(interface);
    interface.handle_request(
        JsonValue(1), "session/new", JsonValue::object_t { });
    const std::string id = created_session_id(sink.snapshot());
    REQUIRE_FALSE(id.empty());

    interface.handle_request(JsonValue(2), "session/prompt",
        object_of({ { "sessionId", JsonValue(id) },
            { "prompt",
                JsonValue::array_t { object_of({ { "type", JsonValue("text") },
                    { "text", JsonValue("go") } }) } } }));
    CHECK(imza::test::wait_until(
        [&] { return contains_line(sink.snapshot(), R"("stopReason")"); }));

    // Chunks before the tool call share one message id; the chunk after
    // it starts a new message. Every chunk has a nonzero id.
    bool saw_zero = false;
    for (const std::string& line : sink.snapshot()) {
        if (line.find("agent_message_chunk") != std::string::npos
            || line.find("agent_thought_chunk") != std::string::npos) {
            if (line.find(R"("messageId":0)") != std::string::npos) {
                saw_zero = true;
            }
        }
    }
    CHECK_FALSE(saw_zero);
}
