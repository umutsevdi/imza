#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "app/application_state.h"
#include "app/flows.h"
#include "conversation/persistence.h"
#include "conversation/session.h"
#include "conversation/session_store.h"
#include "network/json.h"
#include "network/json_io.h"
#include "platform/config.h"
#include "platform/file_lock.h"
#include "test_fs.h"
#include "test_helpers.h"
#include "test_state.h"

namespace {

using JsonValue = imza::JsonValue;

imza::Status load_session(const std::filesystem::path& path,
    imza::Session& session, std::filesystem::path* workspace = nullptr)
{
    imza::LoadedSession loaded;
    const imza::Status status = imza::read_session(path, loaded);
    if (status != imza::Status::OK) {
        return status;
    }
    if (workspace != nullptr) {
        *workspace = loaded.workspace;
    }
    session.restore(std::move(loaded.snapshot));
    return imza::Status::OK;
}

using DataHome = imza::test::IsolatedDataHome;
using imza::test::CurrentDirectory;

// A finished one-tool session: user turn, assistant stub, the request
// applied through Session::apply, and its filled result.
void make_tool_session(const imza::ToolCallRequest& request,
    imza::ToolCall::Result result, imza::Session& source)
{
    source.begin_send("run lua");
    source.append_assistant("model", "off");
    imza::test::append_tool(source, request);
    source.fill_tool_result(request, std::move(result));
    source.finish_session("");
}

// Save `source`, require exactly one archived session, load it back into
// `loaded`, and report the saved path.
void roundtrip(imza::Session& source, imza::Session& loaded,
    std::filesystem::path* archived = nullptr)
{
    REQUIRE(imza::save_session(source) == imza::Status::OK);
    const auto saved = imza::saved_sessions();
    REQUIRE(saved.size() == 1);
    if (archived != nullptr) {
        *archived = saved.front().path;
    }
    REQUIRE(load_session(saved.front().path, loaded) == imza::Status::OK);
}

} // namespace

TEST_CASE("saved sessions continue in place and rewrite the same file")
{
    DataHome home;
    imza::Session source;
    source.set_title("Continued session");
    source.begin_send("hello");
    source.append_assistant("model", "off");
    source.apply(imza::make_delta_event("world"), { });

    REQUIRE(imza::save_session(source) == imza::Status::OK);
    CHECK_FALSE(source.snapshot_for_save());
    auto saved = imza::saved_sessions();
    REQUIRE(saved.size() == 1);
    CHECK(saved.front().path.parent_path() == imza::sessions_dir());
    CHECK(imza::sessions_dir() == imza::data_dir() / "sessions");
    CHECK(saved.front().title == "Continued session");
    const std::filesystem::path saved_path = saved.front().path;
    CHECK(saved_path.stem().string() == source.session_id());

    source.begin_send("follow-up");
    CHECK(source.snapshot_for_save().has_value());
    REQUIRE(imza::save_session(source) == imza::Status::OK);
    saved = imza::saved_sessions();
    REQUIRE(saved.size() == 1);
    CHECK(saved.front().path == saved_path);

    CurrentDirectory directory;
    const auto other = home.root() / "other-workspace";
    std::filesystem::create_directories(other);
    std::error_code ec;
    std::filesystem::current_path(other, ec);
    REQUIRE_FALSE(ec);

    imza::Session loaded;
    std::filesystem::path workspace;
    REQUIRE(load_session(saved_path, loaded, &workspace) == imza::Status::OK);
    CHECK(workspace == directory.original);
    CHECK(loaded.session_id() == saved_path.stem().string());
    const auto loaded_items = loaded.items();
    CHECK(std::get<imza::UserTurn>((*loaded_items)[0]).text == "hello");
    CHECK(
        std::get<imza::AssistantTurn>((*loaded_items)[1]).markdown == "world");

    CHECK(loaded.snapshot_for_save() == std::nullopt);
    loaded.begin_send("parallel continuation");
    REQUIRE(imza::save_session(loaded) == imza::Status::OK);
    saved = imza::saved_sessions();
    REQUIRE(saved.size() == 1);
    CHECK(saved.front().path == saved_path);
    REQUIRE(imza::save_session(loaded) == imza::Status::OK);
    CHECK(imza::saved_sessions().size() == 1);
}

TEST_CASE("a dangling tool call from an interrupted turn heals on restore")
{
    DataHome home;
    imza::Session source;
    source.begin_send("run lua");
    source.append_assistant("model", "off");
    imza::test::append_tool(
        source, imza::ToolCallRequest { "t1", "lua", "{}" });
    // Interrupted mid-tool: no result, saved as-is.
    REQUIRE(imza::save_session(source) == imza::Status::OK);
    const auto saved = imza::saved_sessions();
    REQUIRE(saved.size() == 1);

    imza::Session loaded;
    REQUIRE(load_session(saved.front().path, loaded) == imza::Status::OK);
    // Pending work would block compaction and session switching forever.
    CHECK_FALSE(loaded.has_pending_work());
    CHECK(loaded.phase() == imza::Session::Phase::IDLE);
}

TEST_CASE("empty sessions are not saved")
{
    DataHome home;
    imza::Session session;
    CHECK_FALSE(session.has_items());
    CHECK_FALSE(session.snapshot_for_save());
    CHECK(imza::save_session(session) == imza::Status::OK);
    CHECK(imza::saved_sessions().empty());
}

TEST_CASE("session index is created and rebuilt from session files")
{
    DataHome home;
    imza::Session session;
    session.set_title("Indexed title");
    session.begin_send("hello");

    REQUIRE(imza::save_session(session) == imza::Status::OK);
    const std::filesystem::path index = imza::sessions_dir() / ".index.json";
    REQUIRE(std::filesystem::is_regular_file(index));
    CHECK_FALSE(std::filesystem::exists(index.string() + ".lock"));

    std::error_code ec;
    std::filesystem::remove(index, ec);
    REQUIRE_FALSE(ec);
    const auto rebuilt = imza::saved_sessions();

    REQUIRE(rebuilt.size() == 1);
    CHECK(rebuilt.front().title == "Indexed title");
    CHECK(std::filesystem::is_regular_file(index));

    {
        std::ofstream corrupt(index, std::ios::trunc);
        corrupt << "not json";
    }
    const auto recovered = imza::saved_sessions();
    REQUIRE(recovered.size() == 1);
    CHECK(recovered.front().title == "Indexed title");
    CHECK(imza::delete_saved_session(index)
        == imza::DeleteSessionResult::INVALID_PATH);
}

TEST_CASE("concurrent session saves merge their index entries")
{
    DataHome home;
    constexpr int count = 8;
    std::vector<std::thread> workers;
    std::vector<imza::Status> statuses(count, imza::Status::CONFIG_ERROR);
    workers.reserve(count);
    for (int index = 0; index < count; ++index) {
        workers.emplace_back([index, &statuses] {
            imza::Session session;
            session.set_title("Session " + std::to_string(index));
            session.begin_send("hello");
            statuses[index] = imza::save_session(session);
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    for (const imza::Status status : statuses) {
        CHECK(status == imza::Status::OK);
    }

    const auto saved = imza::saved_sessions();
    CHECK(saved.size() == count);
    REQUIRE_FALSE(saved.empty());
    REQUIRE(imza::delete_saved_session(saved.front().path)
        == imza::DeleteSessionResult::OK);
    CHECK(imza::saved_sessions().size() == count - 1);
}

TEST_CASE("saved sessions retain delegated-agent chat transcripts")
{
    DataHome home;
    imza::Session source;
    source.begin_send("delegate");
    source.append_assistant("model", "off");
    const imza::ToolCallRequest request { "subagent", "{}", "", "call-1" };
    imza::test::append_tool(source, request);
    source.set_tool_subagent_chats(
        request, { { "Agent 1 (research)", "## Assistant\n\nreport" } });
    source.fill_tool_result(
        request, { imza::ToolCall::Result::Kind::OUTPUT, "report" });
    source.finish_session("");

    imza::Session loaded;
    roundtrip(source, loaded);
    const auto loaded_items = loaded.items();
    REQUIRE(loaded_items->size() == 3);
    const auto& call = std::get<imza::ToolCall>((*loaded_items)[2]);
    REQUIRE(call.subagent_chats.size() == 1);
    CHECK(call.subagent_chats[0].title == "Agent 1 (research)");
    CHECK(
        call.subagent_chats[0].transcript.find("report") != std::string::npos);
}

TEST_CASE("lua dispatch log survives session persistence")
{
    DataHome home;
    const imza::ToolCallRequest request { "lua",
        R"json({"script":"print(1)"})json", "", "call-1" };
    imza::ToolCall::Result result { imza::ToolCall::Result::Kind::OUTPUT,
        "1\n" };
    result.dispatch_log
        = { { "read", "/tmp/a", true }, { "sh", "false", false } };

    imza::Session source;
    make_tool_session(request, std::move(result), source);
    imza::Session loaded;
    roundtrip(source, loaded);
    const auto loaded_items = loaded.items();
    REQUIRE(loaded_items->size() == 3);
    const auto& call = std::get<imza::ToolCall>((*loaded_items)[2]);
    REQUIRE(call.result.has_value());
    REQUIRE(call.result->dispatch_log.size() == 2);
    const imza::LuaBindingCall read { "read", "/tmp/a", true };
    const imza::LuaBindingCall shell { "sh", "false", false };
    CHECK(call.result->dispatch_log[0] == read);
    CHECK(call.result->dispatch_log[1] == shell);
}

TEST_CASE("lua return value survives session persistence")
{
    DataHome home;
    const imza::ToolCallRequest request { "lua",
        R"json({"script":"return {a = 1}"})json", "", "call-1" };
    imza::ToolCall::Result result { imza::ToolCall::Result::Kind::OUTPUT, "" };
    result.return_value
        = imza::parse_json(R"json({"a":1,"b":[true,null]})json");

    imza::Session source;
    make_tool_session(request, std::move(result), source);
    imza::Session loaded;
    roundtrip(source, loaded);
    const auto loaded_items = loaded.items();
    const auto& call        = std::get<imza::ToolCall>((*loaded_items)[2]);
    REQUIRE(call.result.has_value());
    REQUIRE(call.result->return_value.has_value());
    const imza::JsonValue& returned = *call.result->return_value;
    const imza::JsonValue* a        = imza::find_member(returned, "a");
    REQUIRE(a != nullptr);
    const int a_value = a->as<int>();
    CHECK(a_value == 1);
    const imza::JsonValue* b = imza::find_member(returned, "b");
    REQUIRE(b != nullptr);
    REQUIRE(b->is_array());
    const auto& b_array = b->get<JsonValue::array_t>();
    REQUIRE(b_array.size() == 2);
    REQUIRE(b_array[0].is_boolean());
    CHECK(b_array[0].get<bool>());
    CHECK(b_array[1].is_null());
}

TEST_CASE("a tool result with invalid utf-8 still loads and replays clean")
{
    DataHome home;
    // A truncated three-byte sequence, as a byte-sliced tool result leaves.
    const char raw[] = "table: \xe2\x9c";
    imza::ToolCall::Result result { imza::ToolCall::Result::Kind::OUTPUT,
        std::string(raw, sizeof(raw) - 1) };

    imza::Session source;
    make_tool_session(
        { "lua", R"json({"script":"print(1)"})json", "", "call-1" },
        std::move(result), source);
    imza::Session loaded;
    roundtrip(source, loaded);

    const auto loaded_items = loaded.items();
    const auto& call        = std::get<imza::ToolCall>((*loaded_items)[2]);
    REQUIRE(call.result.has_value());
    CHECK_FALSE(imza::strip_invalid_utf8(call.result->text).has_value());

    const std::vector<imza::Message> history
        = loaded.build_history("system prompt");
    for (const imza::Message& message : history) {
        CHECK_FALSE(imza::strip_invalid_utf8(message.content).has_value());
    }
}

TEST_CASE("legacy lua result without a return value still loads")
{
    DataHome home;
    const imza::ToolCallRequest request { "lua",
        R"json({"script":"print(1)"})json", "", "call-1" };
    imza::ToolCall::Result result { imza::ToolCall::Result::Kind::OUTPUT,
        "1\n" };

    imza::Session source;
    make_tool_session(request, std::move(result), source);
    imza::Session loaded;
    roundtrip(source, loaded);
    const auto loaded_items = loaded.items();
    const auto& call        = std::get<imza::ToolCall>((*loaded_items)[2]);
    REQUIRE(call.result.has_value());
    CHECK_FALSE(call.result->return_value.has_value());
    CHECK(call.result->text == "1\n");
}

TEST_CASE("lua aggregate diffs survive session persistence")
{
    DataHome home;
    const imza::ToolCallRequest request { "lua",
        R"json({"script":"imza.fs.edit(...)"} )json", "", "call-1" };
    imza::ToolCall::Result result { imza::ToolCall::Result::Kind::OUTPUT, "" };
    imza::DiffView diff;
    diff.file = "/tmp/a.txt";
    diff.rows.push_back({ imza::DiffRow::Kind::SAME, 1, 1, "x", "x" });
    diff.rows.push_back({ imza::DiffRow::Kind::REMOVE, 2, { }, "old", "" });
    diff.rows.push_back({ imza::DiffRow::Kind::ADD, { }, 2, "", "new" });
    result.diffs.push_back(diff);

    imza::Session source;
    make_tool_session(request, std::move(result), source);
    imza::Session loaded;
    roundtrip(source, loaded);
    const auto loaded_items = loaded.items();
    const auto& call        = std::get<imza::ToolCall>((*loaded_items)[2]);
    REQUIRE(call.result.has_value());
    REQUIRE(call.result->diffs.size() == 1);
    CHECK(call.result->diffs[0].file == "/tmp/a.txt");
    REQUIRE(call.result->diffs[0].rows.size() == 3);
    CHECK(call.result->diffs[0].rows[1].kind == imza::DiffRow::Kind::REMOVE);
    CHECK(call.result->diffs[0].rows[1].left == "old");
    CHECK(call.result->diffs[0].rows[2].kind == imza::DiffRow::Kind::ADD);
    CHECK(call.result->diffs[0].rows[2].right == "new");
}

TEST_CASE("canvas charts survive session persistence")
{
    DataHome home;
    const imza::ToolCallRequest request { "lua", "{}", "", "call-1" };
    imza::ToolCall::Result result { imza::ToolCall::Result::Kind::OUTPUT, "" };
    imza::CanvasView pie;
    pie.kind   = imza::CanvasView::Kind::PIE;
    pie.title  = "Share";
    pie.series = { { "a", { 1.0 } }, { "b", { 2.5 } } };
    imza::CanvasView surface;
    surface.kind = imza::CanvasView::Kind::SURFACE;
    surface.grid = { { 1, 2 }, { 3, 4 } };
    result.canvases.push_back(std::move(pie));
    result.canvases.push_back(std::move(surface));

    imza::Session source;
    make_tool_session(request, std::move(result), source);
    imza::Session loaded;
    roundtrip(source, loaded);
    const auto loaded_items = loaded.items();
    const auto& call        = std::get<imza::ToolCall>((*loaded_items)[2]);
    REQUIRE(call.result.has_value());
    REQUIRE(call.result->canvases.size() == 2);
    CHECK(call.result->canvases[0].kind == imza::CanvasView::Kind::PIE);
    CHECK(call.result->canvases[0].title == "Share");
    REQUIRE(call.result->canvases[0].series.size() == 2);
    CHECK(call.result->canvases[0].series[1].label == "b");
    CHECK(call.result->canvases[0].series[1].values[0] == doctest::Approx(2.5));
    CHECK(call.result->canvases[1].kind == imza::CanvasView::Kind::SURFACE);
    REQUIRE(call.result->canvases[1].grid.size() == 2);
    CHECK(call.result->canvases[1].grid[1][0] == doctest::Approx(3.0));
}

TEST_CASE("lua aggregate diffs round-trip SKIP and clamp unknown kinds")
{
    DataHome home;
    const imza::ToolCallRequest request { "lua", "{}", "", "call-1" };
    imza::ToolCall::Result result { imza::ToolCall::Result::Kind::OUTPUT, "" };
    imza::DiffView diff;
    diff.file = "/tmp/a.txt";
    diff.rows.push_back({ imza::DiffRow::Kind::SAME, 1, 1, "x", "x" });
    diff.rows.push_back({ imza::DiffRow::Kind::SKIP, 2, 2,
        "… 5 unchanged line(s) …", "… 5 unchanged line(s) …" });
    diff.rows.push_back({ imza::DiffRow::Kind::REMOVE, 8, { }, "old", "" });
    diff.rows.push_back({ imza::DiffRow::Kind::ADD, { }, 8, "", "new" });
    result.diffs.push_back(diff);

    imza::Session source;
    make_tool_session(request, std::move(result), source);
    std::filesystem::path saved_path;
    imza::Session loaded;
    roundtrip(source, loaded, &saved_path);
    const auto loaded_items = loaded.items();
    const auto& call        = std::get<imza::ToolCall>((*loaded_items)[2]);
    REQUIRE(call.result.has_value());
    REQUIRE(call.result->diffs.size() == 1);
    REQUIRE(call.result->diffs[0].rows.size() == 4);
    CHECK(call.result->diffs[0].rows[1].kind == imza::DiffRow::Kind::SKIP);
    CHECK(call.result->diffs[0].rows[1].left == "… 5 unchanged line(s) …");
    CHECK(call.result->diffs[0].rows[2].kind == imza::DiffRow::Kind::REMOVE);
    CHECK(call.result->diffs[0].rows[3].kind == imza::DiffRow::Kind::ADD);

    // Kinds newer than this build clamps to SAME so old sessions load.
    std::ifstream in(saved_path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string json     = buffer.str();
    const std::size_t at = json.find("\"kind\":3");
    REQUIRE(at != std::string::npos);
    json.replace(at, 8, "\"kind\":9");
    {
        std::ofstream out(saved_path);
        out << json;
    }
    imza::Session clamped;
    REQUIRE(load_session(saved_path, clamped) == imza::Status::OK);
    const auto clamped_items = clamped.items();
    const auto& clamped_call = std::get<imza::ToolCall>((*clamped_items)[2]);
    REQUIRE(clamped_call.result.has_value());
    REQUIRE(clamped_call.result->diffs[0].rows.size() == 4);
    CHECK(clamped_call.result->diffs[0].rows[1].kind
        == imza::DiffRow::Kind::SAME);
}
TEST_CASE("empty title is normalized in both file and index")
{
    DataHome home;
    imza::Session session;
    session.begin_send("hello");
    REQUIRE(imza::save_session(session) == imza::Status::OK);

    const auto saved = imza::saved_sessions();
    REQUIRE(saved.size() == 1);
    CHECK(saved.front().title == "Untitled session");

    imza::Session loaded;
    REQUIRE(load_session(saved.front().path, loaded) == imza::Status::OK);
    CHECK(loaded.title() == "Untitled session");
}

TEST_CASE("non-ASCII workspace paths round-trip as valid UTF-8")
{
    DataHome home;
    CurrentDirectory directory;
    // Narrow literals reach path via the ANSI code page on Windows and
    // mangle the name on disk; build it from the UTF-8 bytes instead.
    const auto workspace
        = imza::path_from_utf8(home.root().generic_string() + "/Eylül çalışma");
    std::filesystem::create_directories(workspace);
    std::error_code ec;
    std::filesystem::current_path(workspace, ec);
    REQUIRE_FALSE(ec);

    imza::Session source;
    source.set_title("Unicode workspace");
    source.begin_send("hello");
    REQUIRE(imza::save_session(source) == imza::Status::OK);

    const auto saved = imza::saved_sessions();
    REQUIRE(saved.size() == 1);
    std::ifstream raw(saved.front().path, std::ios::binary);
    std::stringstream text;
    text << raw.rdbuf();
    const std::string bytes = text.str();
    REQUIRE_FALSE(bytes.empty());
    const bool escaped = bytes.find("Eyl\\u00fcl") != std::string::npos;
    const bool utf8    = bytes.find("Eyl\xc3\xbcl") != std::string::npos;
    const bool encoded = escaped || utf8;
    CHECK(encoded);
    CHECK(bytes.find("\xef\xbf\xbd") == std::string::npos);

    const auto other = home.root() / "other-workspace";
    std::filesystem::create_directories(other);
    std::filesystem::current_path(other, ec);
    REQUIRE_FALSE(ec);

    imza::Session loaded;
    std::filesystem::path loaded_workspace;
    REQUIRE(load_session(saved.front().path, loaded, &loaded_workspace)
        == imza::Status::OK);
    CHECK(loaded_workspace == workspace);
    CHECK(loaded.title() == "Unicode workspace");
}

TEST_CASE("missing or stale workspace falls back to the current directory")
{
    DataHome home;
    CurrentDirectory directory;
    std::filesystem::create_directories(home.root());
    const std::filesystem::path stale = home.root() / "stale.json";
    {
        std::ofstream file(stale, std::ios::binary);
        file << "{\n"
             << "  \"version\": 1,\n"
             << "  \"title\": \"Stale workspace\",\n"
             << "  \"saved_at\": \"2026-09-10 19:17:27\",\n"
             << "  \"mode\": \"build\",\n"
             << "  \"workspace\": \"" << directory.original.generic_string()
             << "/gone-away\",\n"
             << "  \"items\": [{\"type\": \"user\", \"text\": \"hello\"}]\n"
             << "}\n";
    }

    imza::LoadedSession loaded;
    REQUIRE(imza::read_session(stale, loaded) == imza::Status::OK);
    CHECK(loaded.workspace == directory.original);
    REQUIRE(loaded.snapshot.items.size() == 1);
    CHECK(std::get<imza::UserTurn>(loaded.snapshot.items.front()).text
        == "hello");

    const std::filesystem::path legacy = home.root() / "legacy.json";
    {
        std::ofstream file(legacy, std::ios::binary);
        file << "{\n"
             << "  \"title\": \"Legacy format\",\n"
             << "  \"items\": [{\"type\": \"user\", \"text\": \"hi\"}]\n"
             << "}\n";
    }
    REQUIRE(imza::read_session(legacy, loaded) == imza::Status::OK);
    CHECK(loaded.workspace == directory.original);
}

TEST_CASE("malformed field types fail the load instead of crashing")
{
    DataHome home;
    CurrentDirectory directory;
    std::filesystem::create_directories(home.root());
    const std::filesystem::path malformed = home.root() / "malformed.json";
    {
        std::ofstream file(malformed, std::ios::binary);
        file << "{\n"
             << "  \"version\": 1,\n"
             << "  \"workspace\": \"" << directory.original.string() << "\",\n"
             << "  \"items\": [{\"type\": \"tool\", \"id\": \"abc\"}]\n"
             << "}\n";
    }

    imza::LoadedSession loaded;
    CHECK(imza::read_session(malformed, loaded) == imza::Status::JSON_ERROR);
}

TEST_CASE("session id is generated ahead of use and adopts the stem on load")
{
    DataHome home;
    imza::Session session;
    const std::string initial_id = session.session_id();
    CHECK_FALSE(initial_id.empty());
    // The id predates any traffic, so the gateway session header can carry
    // it from the first request of a run.
    session.set_title("Lifecycle test");
    session.begin_send("hello");
    CHECK(session.session_id() == initial_id);
    REQUIRE(imza::save_session(session) == imza::Status::OK);
    const auto saved = imza::saved_sessions();
    REQUIRE(saved.size() == 1);
    CHECK(saved.front().path.stem().string() == initial_id);
    CHECK(session.snapshot_for_save() == std::nullopt);

    // The /new flow reuses the object; a rotated id keeps the fresh
    // conversation from overwriting the archive it left behind.
    const std::string pre_new_id = session.session_id();
    session.restore(imza::SessionSnapshot { });
    CHECK(session.session_id() != pre_new_id);
    CHECK_FALSE(session.has_items());
}

TEST_CASE("locked sessions block loads and deletion, then recover")
{
    DataHome home;
    CurrentDirectory directory;
    imza::Session source;
    source.set_title("Locked session");
    source.begin_send("hello");
    REQUIRE(imza::save_session(source) == imza::Status::OK);
    const auto saved = imza::saved_sessions();
    REQUIRE(saved.size() == 1);
    const auto path = saved.front().path;

    imza::SessionStore store;
    // A lock held by another process makes activation fail and the session
    // stay locked.
    imza::test::ForeignLock foreign(path);

    CHECK(store.is_locked(path));
    CHECK(imza::session_file_locked(path));
    CHECK_FALSE(store.activate(path));
    // Reads refuse a foreign-locked file instead of blocking; the exclusive
    // lock also makes deletion fail.
    imza::LoadedSession loaded;
    CHECK(imza::read_session(path, loaded) == imza::Status::CONFIG_ERROR);
    CHECK(imza::delete_saved_session(path)
        == imza::DeleteSessionResult::REMOVE_FAILED);
    CHECK(imza::saved_sessions().size() == 1);

    // Activating another path releases the first.
    foreign.release();
    CHECK_FALSE(store.is_locked(path));
    REQUIRE(store.activate(path));
    CHECK(store.is_locked(path));
    const auto other = imza::sessions_dir() / "other-session.json";
    REQUIRE(store.activate(other));
    CHECK_FALSE(store.is_locked(path));
    REQUIRE(store.activate(path));
    // The store drops its own guard before deleting; a foreign lock would
    // still make the removal fail.
    CHECK(store.remove(path) == imza::DeleteSessionResult::OK);
    CHECK(imza::saved_sessions().empty());
    store.deactivate();
}

TEST_CASE("native and legacy attachments survive session persistence")
{
    DataHome home;
    const std::string image_bytes("\x89PNG\r\n\x1a\n\0payload", 16);
    const std::string pdf_bytes("%PDF-1.7\n\0payload", 17);
    imza::Session source;
    source.begin_send("review",
        { { "notes.txt", "plain text" },
            { "image.dat", image_bytes, imza::Attachment::Type::IMAGE,
                "image/png" },
            { "document.dat", pdf_bytes, imza::Attachment::Type::PDF,
                "application/pdf" } });

    REQUIRE(imza::save_session(source) == imza::Status::OK);
    const auto saved = imza::saved_sessions();
    REQUIRE(saved.size() == 1);
    std::ifstream file(saved.front().path, std::ios::binary);
    std::stringstream buffer;
    buffer << file.rdbuf();
    const imza::JsonValue root   = imza::parse_json(buffer.str());
    const imza::JsonValue* items = imza::find_member(root, "items");
    REQUIRE(items != nullptr);
    REQUIRE(items->is_array());
    const auto& item_list = items->get<JsonValue::array_t>();
    REQUIRE(item_list.size() >= 1);
    const imza::JsonValue* attachments
        = imza::find_member(item_list[0], "attachments");
    REQUIRE(attachments != nullptr);
    REQUIRE(attachments->is_array());
    const auto& list = attachments->get<JsonValue::array_t>();
    REQUIRE(list.size() == 3);
    const auto str = [](const imza::JsonValue* v) {
        return v != nullptr && v->is_string() ? v->as<std::string>() : "";
    };
    CHECK(str(imza::find_member(list[0], "type")) == "text");
    CHECK(str(imza::find_member(list[0], "content")) == "plain text");
    CHECK(str(imza::find_member(list[1], "type")) == "image");
    CHECK(str(imza::find_member(list[1], "media_type")) == "image/png");
    CHECK(imza::find_member(list[1], "content") != nullptr);
    CHECK(str(imza::find_member(list[1], "content")) != image_bytes);
    CHECK(str(imza::find_member(list[2], "type")) == "pdf");
    CHECK(str(imza::find_member(list[2], "media_type")) == "application/pdf");

    imza::Session loaded;
    REQUIRE(load_session(saved.front().path, loaded) == imza::Status::OK);
    const auto loaded_items = loaded.items();
    const auto& current
        = std::get<imza::UserTurn>(loaded_items->front()).attachments;
    REQUIRE(current.size() == 3);
    CHECK(current[0].type == imza::Attachment::Type::TEXT);
    CHECK(current[0].content == "plain text");
    CHECK(current[1].type == imza::Attachment::Type::IMAGE);
    CHECK(current[1].media_type == "image/png");
    CHECK(current[1].content == image_bytes);
    CHECK(current[2].type == imza::Attachment::Type::PDF);
    CHECK(current[2].media_type == "application/pdf");
    CHECK(current[2].content == pdf_bytes);

    const std::filesystem::path legacy
        = home.root() / "legacy-attachments.json";
    {
        std::ofstream legacy_file(legacy, std::ios::binary);
        legacy_file
            << R"({"items":[{"type":"user","text":"old","attachments":[{"path":"old.txt","content":"legacy text"}]}]})";
    }
    imza::LoadedSession legacy_loaded;
    REQUIRE(imza::read_session(legacy, legacy_loaded) == imza::Status::OK);
    const auto& legacy_attachments
        = std::get<imza::UserTurn>(legacy_loaded.snapshot.items.front())
              .attachments;
    REQUIRE(legacy_attachments.size() == 1);
    CHECK(legacy_attachments[0].path == "old.txt");
    CHECK(legacy_attachments[0].content == "legacy text");
    CHECK(legacy_attachments[0].type == imza::Attachment::Type::TEXT);
    CHECK(legacy_attachments[0].media_type.empty());
}
TEST_CASE("plan document persists across save and restore")
{
    DataHome home;
    imza::Session source;
    source.begin_send("plan this");

    const std::string& skeleton = imza::test::PLAN_SKELETON;
    REQUIRE(source.create_plan(skeleton).empty());
    REQUIRE(source
            .edit_plan("# Requirements\nx", "# Requirements\nauth tokens\n", 1)
            .empty());
    const std::string second = skeleton + "\nextra";
    REQUIRE(source.create_plan(second).empty());
    CHECK(source.plan_doc() == second);

    REQUIRE(imza::save_session(source) == imza::Status::OK);
    const auto saved = imza::saved_sessions();
    REQUIRE(saved.size() == 1);

    imza::Session loaded;
    REQUIRE(load_session(saved.front().path, loaded) == imza::Status::OK);
    const auto loaded_documents = loaded.documents();
    CHECK(loaded_documents->size() == 2);
    CHECK(loaded.plan_doc() == second);
    CHECK(loaded_documents->front().content
        == "# Requirements\nauth tokens\n\n# Approach\nx\n# Changes\nx\n"
           "# Verification\nx");

    CHECK_FALSE(source.create_plan("no headings").empty());
}

TEST_CASE("build decisions persist interleaved with plan revisions")
{
    DataHome home;
    imza::Session source;
    source.begin_send("build this");

    REQUIRE(source.create_plan(imza::test::PLAN_SKELETON).empty());
    REQUIRE(source.append_build_decision("chose A over B").empty());
    REQUIRE(source.append_build_decision("added dependency Z").empty());
    REQUIRE(source.create_plan(imza::test::PLAN_SKELETON + "\nmore").empty());
    REQUIRE(source.append_build_decision("second stint").empty());

    REQUIRE(imza::save_session(source) == imza::Status::OK);
    const auto saved = imza::saved_sessions();
    REQUIRE(saved.size() == 1);

    imza::Session loaded;
    REQUIRE(load_session(saved.front().path, loaded) == imza::Status::OK);
    const auto documents = loaded.documents();
    REQUIRE(documents->size() == 4);
    CHECK(documents->at(0).kind == imza::SessionDoc::Kind::PLAN);
    CHECK(documents->at(1).kind == imza::SessionDoc::Kind::BUILD);
    CHECK(documents->at(1).content == "chose A over B\n\nadded dependency Z");
    CHECK(documents->at(2).kind == imza::SessionDoc::Kind::PLAN);
    CHECK(documents->at(3).kind == imza::SessionDoc::Kind::BUILD);
    CHECK(documents->at(3).content == "second stint");
    CHECK(loaded.plan_doc() == imza::test::PLAN_SKELETON + "\nmore");
}

TEST_CASE("legacy plans-only transcript loads as plan documents")
{
    DataHome home;
    CurrentDirectory directory;
    std::filesystem::create_directories(home.root());
    const std::filesystem::path legacy = home.root() / "legacy-plans.json";
    {
        std::ofstream file(legacy, std::ios::binary);
        file << R"({
  "title": "old",
  "plans": ["# Requirements\nx\n# Approach\nx\n# Changes\nx\n# Verification\nx"],
  "mode": "plan"
})";
    }

    imza::LoadedSession loaded;
    REQUIRE(imza::read_session(legacy, loaded) == imza::Status::OK);
    REQUIRE(loaded.snapshot.documents.size() == 1);
    CHECK(
        loaded.snapshot.documents.front().kind == imza::SessionDoc::Kind::PLAN);
    CHECK(loaded.snapshot.documents.front().content.find("# Requirements")
        != std::string::npos);
}

TEST_CASE("plan document history is not replayed into model context")
{
    imza::Session session;
    const std::string& skeleton = imza::test::PLAN_SKELETON;
    REQUIRE(session.create_plan(skeleton).empty());
    const std::vector<imza::Message> history
        = session.build_history("system prompt");
    // The plan rides in session state only, never in the model history.
    REQUIRE(history.size() == 1);
    CHECK(history[0].type == imza::Message::Type::SYSTEM);
    CHECK(history[0].content == "system prompt");
}

TEST_CASE("plan edit staleness is enforced inside the session")
{
    imza::Session session;
    const std::string& skeleton = imza::test::PLAN_SKELETON;
    // Creating counts as having seen the content.
    REQUIRE(session.create_plan(skeleton).empty());
    CHECK(session.edit_plan("x", "y", 1).empty());

    // The stale-read contract lives in Session, not the bindings.
    imza::Session fresh_reader;
    imza::SessionSnapshot snapshot = session.snapshot();
    fresh_reader.restore(std::move(snapshot));
    // restore marks the state seen, so a restored session edits directly.
    CHECK(fresh_reader.edit_plan("y", "z", 1).empty());
    CHECK(fresh_reader.plan_doc()
        == "# Requirements\nz\n"
           "# Approach\nx\n# Changes\nx\n"
           "# Verification\nx");
}
TEST_CASE("plan submission messages are emitted once per revision in build")
{
    imza::Session session;
    const std::string& skeleton = imza::test::PLAN_SKELETON;
    REQUIRE(session.create_plan(skeleton).empty());

    CHECK_FALSE(imza::Session { }.plan_submission_for_build().has_value());
    CHECK_FALSE(session.plan_submission_for_build().has_value());

    session.set_mode(imza::Session::Mode::BUILD);
    const auto submitted = session.plan_submission_for_build();
    REQUIRE(submitted.has_value());
    CHECK(submitted->find("Plan approved for build: <plan>") == 0);
    CHECK(submitted->find(skeleton) != std::string::npos);
    CHECK(submitted->ends_with("</plan>"));

    CHECK_FALSE(session.plan_submission_for_build().has_value());

    session.set_mode(imza::Session::Mode::PLAN);
    REQUIRE(
        session.edit_plan("# Requirements\nx", "# Requirements\ntokens\n", 1)
            .empty());
    session.set_mode(imza::Session::Mode::BUILD);
    const auto amended = session.plan_submission_for_build();
    REQUIRE(amended.has_value());
    CHECK(amended->find("User has changed the plan: <plan>") == 0);
    CHECK_FALSE(session.plan_submission_for_build().has_value());

    SUBCASE("a restored session re-submits its plan once")
    {
        imza::Session loaded;
        loaded.restore(session.snapshot());
        loaded.set_mode(imza::Session::Mode::BUILD);
        // The watermark is session state, not persisted: a loaded session
        // re-submits once, the safe direction.
        const auto submitted = loaded.plan_submission_for_build();
        REQUIRE(submitted.has_value());
        CHECK(submitted->find("Plan approved for build: <plan>") == 0);
        CHECK_FALSE(loaded.plan_submission_for_build().has_value());
    }

    SUBCASE("restoring into a reused session re-submits its plan once")
    {
        // /new reuses the Session object, so restore() must clear the
        // submission watermark alongside the plan version or the restored
        // plan is silently suppressed.
        session.restore(session.snapshot());
        const auto resubmitted = session.plan_submission_for_build();
        REQUIRE(resubmitted.has_value());
        CHECK_FALSE(session.plan_submission_for_build().has_value());
    }
}

TEST_CASE("restore publishes the mode change so the UI follows it")
{
    imza::Session session;
    REQUIRE(session.mode() == imza::Session::Mode::PLAN);

    imza::SessionSnapshot snapshot;
    snapshot.plan_mode = false;
    int published      = 0;
    const auto subscription
        = session.subscribe_to_mode_change([&published] { ++published; });

    session.restore(std::move(snapshot));
    CHECK(session.mode() == imza::Session::Mode::BUILD);
    CHECK(published == 1);

    // Restoring a conversation in the same mode stays silent.
    session.restore(imza::SessionSnapshot { });
    CHECK(session.mode() == imza::Session::Mode::PLAN);
    CHECK(published == 2);
}

TEST_CASE("plan changes publish the plan signal")
{
    imza::Session session;
    int published = 0;
    const auto subscription
        = session.subscribe_to_documents_change([&published] { ++published; });

    const std::string& skeleton = imza::test::PLAN_SKELETON;
    REQUIRE(session.create_plan(skeleton).empty());
    REQUIRE(
        session.edit_plan("# Requirements\nx", "# Requirements\ny", 1).empty());
    CHECK(published == 2);
}

TEST_CASE("pre-Glaze transcript with unknown members still loads")
{
    DataHome home;
    CurrentDirectory directory;
    std::filesystem::create_directories(home.root());
    const std::filesystem::path legacy = home.root() / "legacy.json";
    {
        std::ofstream file(legacy, std::ios::binary);
        // Shaped like a jsoncpp-era transcript: key order differs, an
        // unknown member ("future_field") is present, and the tool result
        // carries the pre-canvas member set.
        file << R"({
  "items": [
    {"future_field": true, "type": "user", "text": "hello",
     "attachments": []},
    {"type": "assistant", "markdown": "answer", "reasoning": "",
     "reasoning_signature": "", "model": "m", "reasoning_effort": "off"},
    {"type": "tool", "id": 0, "call_id": "c1", "name": "lua",
     "args": "{}", "result_kind": 0, "result": "out"}
  ],
  "version": 1,
  "title": "legacy",
  "todo": [{"content": "task", "status": 1}],
  "plans": [],
  "compacted_summary": "",
  "compacted_item_count": 0,
  "mode": "build"
})";
    }

    imza::LoadedSession loaded;
    REQUIRE(imza::read_session(legacy, loaded) == imza::Status::OK);
    REQUIRE(loaded.snapshot.items.size() == 3);
    const auto* user = std::get_if<imza::UserTurn>(&loaded.snapshot.items[0]);
    REQUIRE(user != nullptr);
    CHECK(user->text == "hello");
    const auto* assistant
        = std::get_if<imza::AssistantTurn>(&loaded.snapshot.items[1]);
    REQUIRE(assistant != nullptr);
    CHECK(assistant->markdown == "answer");
    const auto* tool = std::get_if<imza::ToolCall>(&loaded.snapshot.items[2]);
    REQUIRE(tool != nullptr);
    REQUIRE(tool->result.has_value());
    CHECK(tool->result->text == "out");
    CHECK(loaded.snapshot.todo.items.size() == 1);
    CHECK(loaded.snapshot.todo.items[0].content == "task");
}

TEST_CASE("notices survive session persistence")
{
    DataHome home;
    imza::Session source;
    source.set_title("with a notice");
    source.begin_send("hello");
    source.append_assistant("model", "off");
    source.apply(imza::make_delta_event("world"), { });
    source.finish_session("");
    source.append_notice("Working directory changed to /tmp/target");

    imza::Session loaded;
    roundtrip(source, loaded);
    const auto items = loaded.items();
    REQUIRE(items->size() == 3);
    const auto* notice = std::get_if<imza::Notice>(&items->back());
    REQUIRE(notice != nullptr);
    CHECK(notice->text == "Working directory changed to /tmp/target");
}
