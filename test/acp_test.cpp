#include "acp/adapter.h"
#include "acp/protocol.h"
#include "acp/updates.h"
#include "acp/wire.h"
#include "app/flows.h"
#include "conversation/persistence.h"
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

TEST_CASE("wire parses and dumps JSON-RPC frames")
{
    imza::acp::Message message;
    CHECK(imza::acp::parse_frame(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        message));
    CHECK(message.method == "initialize");
    CHECK(message.id.is_number());
    CHECK(message.id.get<double>() == doctest::Approx(1.0));

    // Zed sends UUID string ids; both forms must parse and survive.
    CHECK(imza::acp::parse_frame(
        R"({"jsonrpc":"2.0","id":"9afa31b2-e222-4a8b","method":"x"})",
        message));
    CHECK(message.id.is_string());
    CHECK(message.id.get<std::string>() == "9afa31b2-e222-4a8b");

    CHECK(imza::acp::parse_frame(
        R"({"jsonrpc":"2.0","method":"session/cancel","params":{}})", message));
    CHECK(message.method == "session/cancel");
    CHECK(message.id.is_null());

    CHECK(imza::acp::parse_frame(
        R"({"jsonrpc":"2.0","id":7,"result":{"stopReason":"end_turn"}})",
        message));
    CHECK(message.result.has_value());

    CHECK_FALSE(imza::acp::parse_frame("not json", message));
    CHECK_FALSE(imza::acp::parse_frame(
        R"({"id":1,"method":"x"})", message)); // missing jsonrpc
    CHECK_FALSE(imza::acp::parse_frame(
        R"({"jsonrpc":"2.0"})", message)); // neither method nor result

    const std::string request = imza::acp::dump_request(
        JsonValue(3.0), "session/prompt", JsonValue::object_t { });
    CHECK(request.find(R"("id":3)") != std::string::npos);
    // String ids echo verbatim.
    const std::string echoed
        = imza::acp::dump_response(JsonValue("abc-1"), JsonValue::object_t { });
    CHECK(echoed.find(R"("id":"abc-1")") != std::string::npos);
    CHECK(request.find("\"method\":\"session/prompt\"") != std::string::npos);
    CHECK(request.find('\n') == std::string::npos);
}

TEST_CASE("initialize reports agent capabilities and terminal auth")
{
    const JsonValue params = object_of({
        { "protocolVersion", JsonValue(1.0) },
        { "clientCapabilities",
            object_of({ { "fs",
                object_of({ { "readTextFile", JsonValue(true) } }) } }) },
        { "clientInfo", object_of({ { "name", JsonValue("zed") } }) },
    });
    const imza::acp::InitializeRequest request
        = imza::acp::InitializeRequest::parse(params);
    CHECK(request.client_name == "zed");
    CHECK(request.client.fs_read_text_file);

    const JsonValue result   = imza::acp::dump_initialize_result(request);
    const JsonValue* version = imza::find_member(result, "protocolVersion");
    REQUIRE(version != nullptr);
    CHECK(version->get<double>() == doctest::Approx(1.0));
    const JsonValue* agent = imza::find_member(result, "agentCapabilities");
    REQUIRE(agent != nullptr);
    const JsonValue* load = imza::find_member(*agent, "loadSession");
    REQUIRE(load != nullptr);
    CHECK(load->get<bool>());
    // No authMethods: provider sign-in stays in the TUI; advertising a
    // terminal method makes clients relaunch for interactive login.
    CHECK(imza::find_member(result, "authMethods") == nullptr);
}

TEST_CASE("unknown methods return -32601; extension requests too")
{
    imza::test::PostPump pump;
    auto state = imza::test::make_test_state(pump.fn());
    WireSink sink;
    imza::acp::AcpInterface interface(*state, { });
    interface.set_writer([&sink](std::string_view line) { sink.write(line); });

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
    auto state = imza::test::make_test_state(pump.fn());
    WireSink sink;
    imza::acp::AcpInterface interface(*state, { });
    interface.set_writer([&sink](std::string_view line) { sink.write(line); });

    interface.handle_request(
        JsonValue(1), "session/new", JsonValue::object_t { });
    auto lines = sink.snapshot();
    CHECK(contains_line(lines, "\"sessionId\":\""));
    CHECK(contains_line(lines, "\"configOptions\""));
    CHECK(contains_line(lines, "\"category\":\"mode\""));

    CHECK(contains_line(lines, "\"availableCommands\""));

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
    REQUIRE(request->options.size() == 3);
    CHECK(request->options[0].kind == "allow_once");
    CHECK(request->options[1].kind == "allow_always");
    CHECK(request->options[2].kind == "reject_once");

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

    const std::vector<imza::SavedSession> saved
        = imza::saved_sessions_of(imza::SessionType::ACP);
    CHECK_FALSE(saved.empty());
    for (const imza::SavedSession& entry : saved) {
        CHECK(entry.type == imza::SessionType::ACP);
    }
}

TEST_CASE("cancellation answers stopReason cancelled")
{
    std::atomic<bool> gate { false };
    std::atomic<bool> turn_started { false };
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

TEST_CASE("session ids round-trip: new, list, load")
{
    imza::test::PostPump pump;
    auto state = imza::test::make_test_state(pump.fn());
    WireSink sink;
    imza::acp::AcpInterface interface(*state, { });
    interface.set_writer([&sink](std::string_view line) { sink.write(line); });

    interface.handle_request(
        JsonValue(1), "session/new", JsonValue::object_t { });
    const std::string created = created_session_id(sink.snapshot());
    REQUIRE_FALSE(created.empty());
    // The protocol id is imza's own session id, never a synthetic
    // counter: it is the stem the session saves under, so a client that
    // remembers it can load the conversation back after a restart.
    CHECK(created.find("imza-") != 0);

    interface.handle_request(JsonValue(2), "session/load",
        object_of({ { "sessionId", JsonValue("no-such-session") } }));
    CHECK(contains_line(sink.snapshot(), "-32002"));
}

TEST_CASE("session/load restores a saved ACP session without crashing")
{
    imza::test::PostPump pump;
    auto state = imza::test::make_test_state(pump.fn());
    WireSink sink;
    imza::acp::AcpInterface interface(*state, { });
    interface.set_writer([&sink](std::string_view line) { sink.write(line); });

    auto saved_state = imza::test::make_test_state(pump.fn());
    {
        imza::SessionSnapshot snapshot;
        snapshot.session_type = imza::SessionType::ACP;
        saved_state->session->restore(std::move(snapshot));
        saved_state->session->begin_send("hello");
        saved_state->session->append_assistant("m", "default");
        saved_state->session->apply(
            imza::make_delta_event("hi there"), imza::ModelPricing { });
        saved_state->session->apply(
            imza::make_done_event(), imza::ModelPricing { });
        saved_state->session->finish_session("");
    }
    REQUIRE(imza::save_session(*saved_state->session) == imza::Status::OK);
    const std::string id = saved_state->session->session_id();

    interface.handle_request(JsonValue(1), "session/load",
        object_of({ { "sessionId", JsonValue(id) } }));
    const auto lines = sink.snapshot();
    CHECK(contains_line(lines, "\"result\""));
    CHECK(contains_line(lines, "user_message_chunk"));
    CHECK(contains_line(lines, "agent_message_chunk"));
    CHECK_FALSE(contains_line(lines, "-32002"));
}
