#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <thread>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "app/application_state.h"
#include "app/flows.h"
#include "conversation/persistence.h"
#include "conversation/session.h"
#include "conversation/session_store.h"
#include "network/json_io.h"
#include "platform/config.h"
#include "platform/file_lock.h"

namespace {

struct DataHome {
    std::filesystem::path path
        = std::filesystem::temp_directory_path() / "imza_session_store_test";
    std::string previous;
    bool had_previous = false;

    DataHome()
    {
        if (const char* value = std::getenv("XDG_DATA_HOME")) {
            previous     = value;
            had_previous = true;
        }
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
#ifndef _WIN32
        setenv("XDG_DATA_HOME", path.c_str(), 1);
#endif
    }

    ~DataHome()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
#ifndef _WIN32
        if (had_previous) {
            setenv("XDG_DATA_HOME", previous.c_str(), 1);
        } else {
            unsetenv("XDG_DATA_HOME");
        }
#endif
    }
};

struct CurrentDirectory {
    std::filesystem::path original = std::filesystem::current_path();

    ~CurrentDirectory()
    {
        std::error_code ec;
        std::filesystem::current_path(original, ec);
    }
};

} // namespace

TEST_CASE("CLI session list aligns columns without terminal tabs")
{
    const std::vector<imza::SavedSession> sessions {
        { "1789291986542-c06df75e.json", "Long title", "2026-09-13 12:33:06" },
        { "short.json", "Other title", "2026-09-13 12:25:53" },
    };

    CHECK(imza::format_session_list(sessions)
        == "SESSION ID              SAVED                TITLE\n"
           "1789291986542-c06df75e  2026-09-13 12:33:06  Long title\n"
           "short                   2026-09-13 12:25:53  Other title\n");
}
TEST_CASE("saved sessions continue in place and rewrite the same file")
{
#ifdef _WIN32
    return;
#else
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

    // A re-save of the same run rewrites its own file.
    source.begin_send("follow-up");
    CHECK(source.snapshot_for_save().has_value());
    REQUIRE(imza::save_session(source) == imza::Status::OK);
    saved = imza::saved_sessions();
    REQUIRE(saved.size() == 1);
    CHECK(saved.front().path == saved_path);

    CurrentDirectory directory;
    const auto other = home.path / "other-workspace";
    std::filesystem::create_directories(other);
    std::error_code ec;
    std::filesystem::current_path(other, ec);
    REQUIRE_FALSE(ec);

    imza::Session loaded;
    std::filesystem::path workspace;
    REQUIRE(
        imza::load_session(saved_path, loaded, &workspace) == imza::Status::OK);
    CHECK(workspace == directory.original);
    // A resumed session adopts the source file stem and saves back into the
    // same file once dirtied.
    CHECK(loaded.session_id() == saved_path.stem().string());
    CHECK(std::get<imza::UserTurn>(loaded.items()[0]).text == "hello");
    CHECK(std::get<imza::AssistantTurn>(loaded.items()[1]).markdown == "world");

    CHECK(loaded.snapshot_for_save() == std::nullopt);
    loaded.begin_send("parallel continuation");
    REQUIRE(imza::save_session(loaded) == imza::Status::OK);
    saved = imza::saved_sessions();
    REQUIRE(saved.size() == 1);
    CHECK(saved.front().path == saved_path);
    REQUIRE(imza::save_session(loaded) == imza::Status::OK);
    CHECK(imza::saved_sessions().size() == 1);
#endif
}

TEST_CASE("empty sessions are not saved")
{
#ifdef _WIN32
    return;
#else
    DataHome home;
    imza::Session session;
    CHECK_FALSE(session.has_items());
    CHECK_FALSE(session.snapshot_for_save());
    CHECK(imza::save_session(session) == imza::Status::OK);
    CHECK(imza::saved_sessions().empty());
#endif
}

TEST_CASE("session index is created and rebuilt from session files")
{
#ifdef _WIN32
    return;
#else
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
#endif
}

TEST_CASE("concurrent session saves merge their index entries")
{
#ifdef _WIN32
    return;
#else
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
#endif
}

TEST_CASE("saved sessions retain delegated-agent chat transcripts")
{
#ifdef _WIN32
    return;
#else
    DataHome home;
    imza::Session source;
    source.begin_send("delegate");
    source.append_assistant("model", "off");
    const imza::ToolCallRequest request { "subagent", "{}", "", "call-1" };
    source.append_tool(request);
    source.set_tool_subagent_chats(
        request, { { "Agent 1 (research)", "## Assistant\n\nreport" } });
    source.fill_tool_result(
        request, { imza::ToolCall::Result::Kind::OUTPUT, "report" });
    source.finish_session("");

    REQUIRE(imza::save_session(source) == imza::Status::OK);
    const auto saved = imza::saved_sessions();
    REQUIRE(saved.size() == 1);
    imza::Session loaded;
    REQUIRE(imza::load_session(saved.front().path, loaded) == imza::Status::OK);
    REQUIRE(loaded.items().size() == 3);
    const auto& call = std::get<imza::ToolCall>(loaded.items()[2]);
    REQUIRE(call.subagent_chats.size() == 1);
    CHECK(call.subagent_chats[0].title == "Agent 1 (research)");
    CHECK(
        call.subagent_chats[0].transcript.find("report") != std::string::npos);
#endif
}

TEST_CASE("lua dispatch log survives session persistence")
{
#ifdef _WIN32
    return;
#else
    DataHome home;
    imza::Session source;
    source.begin_send("run lua");
    source.append_assistant("model", "off");
    const imza::ToolCallRequest request { "lua",
        R"json({"script":"print(1)"})json", "", "call-1" };
    source.append_tool(request);
    imza::ToolCall::Result result { imza::ToolCall::Result::Kind::OUTPUT,
        "1\n" };
    result.dispatch_log
        = { { "read", "/tmp/a", true }, { "sh", "false", false } };
    source.fill_tool_result(request, std::move(result));
    source.finish_session("");

    REQUIRE(imza::save_session(source) == imza::Status::OK);
    const auto saved = imza::saved_sessions();
    REQUIRE(saved.size() == 1);
    imza::Session loaded;
    REQUIRE(imza::load_session(saved.front().path, loaded) == imza::Status::OK);
    REQUIRE(loaded.items().size() == 3);
    const auto& call = std::get<imza::ToolCall>(loaded.items()[2]);
    REQUIRE(call.result.has_value());
    REQUIRE(call.result->dispatch_log.size() == 2);
    const imza::LuaBindingCall read { "read", "/tmp/a", true };
    const imza::LuaBindingCall shell { "sh", "false", false };
    CHECK(call.result->dispatch_log[0] == read);
    CHECK(call.result->dispatch_log[1] == shell);
#endif
}

TEST_CASE("lua return value survives session persistence")
{
#ifdef _WIN32
    return;
#else
    DataHome home;
    imza::Session source;
    source.begin_send("run lua");
    source.append_assistant("model", "off");
    const imza::ToolCallRequest request { "lua",
        R"json({"script":"return {a = 1}"})json", "", "call-1" };
    source.append_tool(request);
    imza::ToolCall::Result result { imza::ToolCall::Result::Kind::OUTPUT, "" };
    result.return_value
        = imza::parse_json(R"json({"a":1,"b":[true,null]})json");
    source.fill_tool_result(request, std::move(result));
    source.finish_session("");

    REQUIRE(imza::save_session(source) == imza::Status::OK);
    const auto saved = imza::saved_sessions();
    REQUIRE(saved.size() == 1);
    imza::Session loaded;
    REQUIRE(imza::load_session(saved.front().path, loaded) == imza::Status::OK);
    const auto& call = std::get<imza::ToolCall>(loaded.items()[2]);
    REQUIRE(call.result.has_value());
    REQUIRE(call.result->return_value.has_value());
    CHECK((*call.result->return_value)["a"].asInt() == 1);
    REQUIRE((*call.result->return_value)["b"].isArray());
    CHECK((*call.result->return_value)["b"][0].asBool());
    CHECK((*call.result->return_value)["b"][1].isNull());
#endif
}

TEST_CASE("legacy lua result without a return value still loads")
{
#ifdef _WIN32
    return;
#else
    DataHome home;
    imza::Session source;
    source.begin_send("run lua");
    source.append_assistant("model", "off");
    const imza::ToolCallRequest request { "lua",
        R"json({"script":"print(1)"})json", "", "call-1" };
    source.append_tool(request);
    imza::ToolCall::Result result { imza::ToolCall::Result::Kind::OUTPUT,
        "1\n" };
    source.fill_tool_result(request, std::move(result));
    source.finish_session("");

    REQUIRE(imza::save_session(source) == imza::Status::OK);
    const auto saved = imza::saved_sessions();
    REQUIRE(saved.size() == 1);
    imza::Session loaded;
    REQUIRE(imza::load_session(saved.front().path, loaded) == imza::Status::OK);
    const auto& call = std::get<imza::ToolCall>(loaded.items()[2]);
    REQUIRE(call.result.has_value());
    CHECK_FALSE(call.result->return_value.has_value());
    CHECK(call.result->text == "1\n");
#endif
}

TEST_CASE("lua aggregate diffs survive session persistence")
{
#ifdef _WIN32
    return;
#else
    DataHome home;
    imza::Session source;
    source.begin_send("run lua");
    source.append_assistant("model", "off");
    const imza::ToolCallRequest request { "lua",
        R"json({"script":"imza.fs.edit(...)"} )json", "", "call-1" };
    source.append_tool(request);
    imza::ToolCall::Result result { imza::ToolCall::Result::Kind::OUTPUT, "" };
    imza::DiffView diff;
    diff.file = "/tmp/a.txt";
    diff.rows.push_back({ imza::DiffRow::Kind::SAME, 1, 1, "x", "x" });
    diff.rows.push_back({ imza::DiffRow::Kind::REMOVE, 2, { }, "old", "" });
    diff.rows.push_back({ imza::DiffRow::Kind::ADD, { }, 2, "", "new" });
    result.diffs.push_back(diff);
    source.fill_tool_result(request, std::move(result));
    source.finish_session("");

    REQUIRE(imza::save_session(source) == imza::Status::OK);
    const auto saved = imza::saved_sessions();
    REQUIRE(saved.size() == 1);
    imza::Session loaded;
    REQUIRE(imza::load_session(saved.front().path, loaded) == imza::Status::OK);
    const auto& call = std::get<imza::ToolCall>(loaded.items()[2]);
    REQUIRE(call.result.has_value());
    REQUIRE(call.result->diffs.size() == 1);
    CHECK(call.result->diffs[0].file == "/tmp/a.txt");
    REQUIRE(call.result->diffs[0].rows.size() == 3);
    CHECK(call.result->diffs[0].rows[1].kind == imza::DiffRow::Kind::REMOVE);
    CHECK(call.result->diffs[0].rows[1].left == "old");
    CHECK(call.result->diffs[0].rows[2].kind == imza::DiffRow::Kind::ADD);
    CHECK(call.result->diffs[0].rows[2].right == "new");
#endif
}

TEST_CASE("lua aggregate diffs round-trip SKIP and clamp unknown kinds")
{
#ifdef _WIN32
    return;
#else
    DataHome home;
    imza::Session source;
    source.begin_send("run lua");
    source.append_assistant("model", "off");
    const imza::ToolCallRequest request { "lua", "{}", "", "call-1" };
    source.append_tool(request);
    imza::ToolCall::Result result { imza::ToolCall::Result::Kind::OUTPUT, "" };
    imza::DiffView diff;
    diff.file = "/tmp/a.txt";
    diff.rows.push_back({ imza::DiffRow::Kind::SAME, 1, 1, "x", "x" });
    diff.rows.push_back({ imza::DiffRow::Kind::SKIP, 2, 2,
        "… 5 unchanged line(s) …", "… 5 unchanged line(s) …" });
    diff.rows.push_back({ imza::DiffRow::Kind::REMOVE, 8, { }, "old", "" });
    diff.rows.push_back({ imza::DiffRow::Kind::ADD, { }, 8, "", "new" });
    result.diffs.push_back(diff);
    source.fill_tool_result(request, std::move(result));
    source.finish_session("");

    REQUIRE(imza::save_session(source) == imza::Status::OK);
    const auto saved = imza::saved_sessions();
    REQUIRE(saved.size() == 1);
    imza::Session loaded;
    REQUIRE(imza::load_session(saved.front().path, loaded) == imza::Status::OK);
    const auto& call = std::get<imza::ToolCall>(loaded.items()[2]);
    REQUIRE(call.result.has_value());
    REQUIRE(call.result->diffs.size() == 1);
    REQUIRE(call.result->diffs[0].rows.size() == 4);
    CHECK(call.result->diffs[0].rows[1].kind == imza::DiffRow::Kind::SKIP);
    CHECK(call.result->diffs[0].rows[1].left == "… 5 unchanged line(s) …");
    CHECK(call.result->diffs[0].rows[2].kind == imza::DiffRow::Kind::REMOVE);
    CHECK(call.result->diffs[0].rows[3].kind == imza::DiffRow::Kind::ADD);

    // Kinds newer than this build clamps to SAME so old sessions load.
    std::ifstream in(saved.front().path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string json     = buffer.str();
    const std::size_t at = json.find("\"kind\":3");
    REQUIRE(at != std::string::npos);
    json.replace(at, 8, "\"kind\":9");
    {
        std::ofstream out(saved.front().path);
        out << json;
    }
    imza::Session clamped;
    REQUIRE(
        imza::load_session(saved.front().path, clamped) == imza::Status::OK);
    const auto& clamped_call = std::get<imza::ToolCall>(clamped.items()[2]);
    REQUIRE(clamped_call.result.has_value());
    REQUIRE(clamped_call.result->diffs[0].rows.size() == 4);
    CHECK(clamped_call.result->diffs[0].rows[1].kind
        == imza::DiffRow::Kind::SAME);
#endif
}
TEST_CASE("empty title is normalized in both file and index")
{
#ifdef _WIN32
    return;
#else
    DataHome home;
    imza::Session session;
    session.begin_send("hello");
    REQUIRE(imza::save_session(session) == imza::Status::OK);

    const auto saved = imza::saved_sessions();
    REQUIRE(saved.size() == 1);
    CHECK(saved.front().title == "Untitled session");

    imza::Session loaded;
    REQUIRE(imza::load_session(saved.front().path, loaded) == imza::Status::OK);
    CHECK(loaded.title() == "Untitled session");
#endif
}

TEST_CASE("non-ASCII workspace paths round-trip as valid UTF-8")
{
#ifdef _WIN32
    return;
#else
    DataHome home;
    CurrentDirectory directory;
    const auto workspace = home.path / "Eylül çalışma";
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

    const auto other = home.path / "other-workspace";
    std::filesystem::create_directories(other);
    std::filesystem::current_path(other, ec);
    REQUIRE_FALSE(ec);

    imza::Session loaded;
    std::filesystem::path loaded_workspace;
    REQUIRE(imza::load_session(saved.front().path, loaded, &loaded_workspace)
        == imza::Status::OK);
    CHECK(loaded_workspace == workspace);
    CHECK(loaded.title() == "Unicode workspace");
#endif
}

TEST_CASE("missing or stale workspace falls back to the current directory")
{
#ifdef _WIN32
    return;
#else
    DataHome home;
    CurrentDirectory directory;
    std::filesystem::create_directories(home.path);
    const std::filesystem::path stale = home.path / "stale.json";
    {
        std::ofstream file(stale, std::ios::binary);
        file << "{\n"
             << "  \"version\": 1,\n"
             << "  \"title\": \"Stale workspace\",\n"
             << "  \"saved_at\": \"2026-09-10 19:17:27\",\n"
             << "  \"mode\": \"build\",\n"
             << "  \"workspace\": \"" << directory.original.string()
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

    const std::filesystem::path legacy = home.path / "legacy.json";
    {
        std::ofstream file(legacy, std::ios::binary);
        file << "{\n"
             << "  \"title\": \"Legacy format\",\n"
             << "  \"items\": [{\"type\": \"user\", \"text\": \"hi\"}]\n"
             << "}\n";
    }
    REQUIRE(imza::read_session(legacy, loaded) == imza::Status::OK);
    CHECK(loaded.workspace == directory.original);
#endif
}

TEST_CASE("malformed field types fail the load instead of crashing")
{
#ifdef _WIN32
    return;
#else
    DataHome home;
    CurrentDirectory directory;
    std::filesystem::create_directories(home.path);
    const std::filesystem::path malformed = home.path / "malformed.json";
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
#endif
}

TEST_CASE("CLI opens and removes saved sessions by ID")
{
#ifdef _WIN32
    return;
#else
    DataHome home;
    imza::Session source;
    source.begin_send("remember this");
    REQUIRE(imza::save_session(source) == imza::Status::OK);
    const auto saved = imza::saved_sessions();
    REQUIRE(saved.size() == 1);
    std::string id = saved.front().path.stem().string();

    char program[]    = "imza";
    char directory[]  = ".";
    char session[]    = "--session";
    char* open_argv[] = { program, directory, session, id.data() };
    const imza::CliResult open_result = imza::run_cli(4, open_argv);

    CHECK(open_result.continue_as_interactive);
    CHECK(open_result.exit_code == 0);
    REQUIRE(open_result.working_directory.has_value());
    CHECK(*open_result.working_directory == std::filesystem::path("."));
    REQUIRE(open_result.session_path.has_value());
    CHECK(*open_result.session_path == saved.front().path);

    char ask[]                 = "--ask";
    char query[]               = "continue";
    char* one_shot_argv[]      = { program, ask, query, session, id.data() };
    const auto one_shot_result = imza::run_cli(5, one_shot_argv);

    REQUIRE(one_shot_result.one_shot.has_value());
    CHECK(one_shot_result.one_shot->mode == imza::OneShotRequest::Mode::ASK);
    CHECK(one_shot_result.session_path == saved.front().path);

    char remove[]       = "rm";
    char* remove_argv[] = { program, session, remove, id.data() };
    const imza::CliResult remove_result = imza::run_cli(4, remove_argv);

    CHECK_FALSE(remove_result.continue_as_interactive);
    CHECK(remove_result.exit_code == 0);
    CHECK(imza::saved_sessions().empty());
#endif
}

TEST_CASE("session id is generated ahead of use and adopts the stem on load")
{
#ifdef _WIN32
    return;
#else
    DataHome home;
    imza::Session session;
    const std::string initial_id = session.session_id();
    CHECK_FALSE(initial_id.empty());
    // The id predates any traffic, so it can be attached as the gateway
    // session header from the very first request of a run.
    session.set_title("Lifecycle test");
    session.begin_send("hello");
    CHECK(session.session_id() == initial_id);
    REQUIRE(imza::save_session(session) == imza::Status::OK);
    const auto saved = imza::saved_sessions();
    REQUIRE(saved.size() == 1);
    CHECK(saved.front().path.stem().string() == initial_id);
    CHECK(session.snapshot_for_save() == std::nullopt);

    // Starting a new session in place (the /new flow) rotates the id, so
    // the fresh conversation cannot overwrite the archive it left behind.
    const std::string pre_new_id = session.session_id();
    session.restore(imza::SessionSnapshot { });
    CHECK(session.session_id() != pre_new_id);
    CHECK_FALSE(session.has_items());
#endif
}

TEST_CASE("locked sessions block loads and deletion, then recover")
{
#ifdef _WIN32
    return;
#else
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
    // A lock held by another process (simulated with a raw handle) makes
    // activation fail and the session stay locked.
    auto foreign = imza::acquire_file_lock(imza::lock_path_for(path),
        imza::FileLockRequest { imza::FileLockMode::EXCLUSIVE, false });
    REQUIRE(std::holds_alternative<imza::FileLock>(foreign));

    CHECK(store.is_locked(path));
    CHECK_FALSE(store.activate(path));
    CHECK(imza::session_file_locked(path));
    // Reads refuse a foreign-locked file instead of blocking, and the
    // exclusive lock also makes deletion fail.
    imza::LoadedSession loaded;
    CHECK(imza::read_session(path, loaded) == imza::Status::CONFIG_ERROR);
    CHECK(imza::delete_saved_session(path)
        == imza::DeleteSessionResult::REMOVE_FAILED);
    CHECK(imza::saved_sessions().size() == 1);

    // After the foreign lock is dropped the same probe succeeds and the
    // session can be activated; activating another path releases the first.
    foreign = std::variant<imza::FileLock, imza::FileLockError> {
        imza::FileLockError { }
    };
    CHECK_FALSE(store.is_locked(path));
    REQUIRE(store.activate(path));
    CHECK(store.is_locked(path));
    const auto other = imza::sessions_dir() / "other-session.json";
    REQUIRE(store.activate(other));
    // The previous guard was released, so the session is free again.
    CHECK_FALSE(store.is_locked(path));
    REQUIRE(store.activate(path));
    // Deleting the active session through the store drops the guard first,
    // so the removal succeeds; a foreign lock would still make it fail.
    CHECK(store.remove(path) == imza::DeleteSessionResult::OK);
    CHECK(imza::saved_sessions().empty());
    store.deactivate();
#endif
}

TEST_CASE("switch_session locks the target and reports foreign locks")
{
#ifdef _WIN32
    return;
#else
    DataHome home;
    CurrentDirectory directory;
    const auto immediate = [](std::function<void()> task) { task(); };
    auto state = imza::make_application_state(immediate, imza::Config { });
    state->session->set_title("Current");
    state->session->begin_send("hello");
    state->session->append_assistant("model", "off");
    state->session->apply(imza::make_delta_event("world"), { });
    state->session->finish_session("");
    REQUIRE(imza::save_session(*state->session) == imza::Status::OK);
    const std::string current_stem = state->session->session_id();

    imza::Session target;
    target.set_title("Target");
    target.begin_send("hola");
    REQUIRE(imza::save_session(target) == imza::Status::OK);
    const auto sessions = imza::saved_sessions();
    REQUIRE(sessions.size() == 2);
    const auto target_path = std::find_if(
        sessions.begin(), sessions.end(), [&](const auto& entry) {
            return entry.title == "Target";
        })->path;

    auto foreign = imza::acquire_file_lock(imza::lock_path_for(target_path),
        imza::FileLockRequest { imza::FileLockMode::EXCLUSIVE, false });
    REQUIRE(std::holds_alternative<imza::FileLock>(foreign));

    imza::switch_session(*state, target_path);
    CHECK(state->session->title() == "Current");
    CHECK(
        state->session->error() == "Session is open in another imza process.");
    CHECK(state->session->session_id() == current_stem);
    CHECK(imza::saved_sessions().size() == 2);

    foreign = std::variant<imza::FileLock, imza::FileLockError> {
        imza::FileLockError { }
    };
    imza::switch_session(*state, target_path);
    CHECK(state->session->title() == "Target");
    CHECK(state->session->error().empty());
    CHECK(state->session->session_id() == target_path.stem().string());

    state->session->begin_send("grew the target in place");
    REQUIRE(imza::save_session(*state->session) == imza::Status::OK);
    CHECK(imza::saved_sessions().size() == 2);
#endif
}

TEST_CASE("native and legacy attachments survive session persistence")
{
#ifdef _WIN32
    return;
#else
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
    const Json::Value root         = imza::parse_json(buffer.str());
    const Json::Value& attachments = root["items"][0]["attachments"];
    REQUIRE(attachments.size() == 3);
    CHECK(attachments[0]["type"].asString() == "text");
    CHECK(attachments[0]["content"].asString() == "plain text");
    CHECK(attachments[1]["type"].asString() == "image");
    CHECK(attachments[1]["media_type"].asString() == "image/png");
    CHECK(attachments[1].isMember("content"));
    CHECK(attachments[1]["content"].asString() != image_bytes);
    CHECK(attachments[2]["type"].asString() == "pdf");
    CHECK(attachments[2]["media_type"].asString() == "application/pdf");

    imza::Session loaded;
    REQUIRE(imza::load_session(saved.front().path, loaded) == imza::Status::OK);
    const auto& current
        = std::get<imza::UserTurn>(loaded.items().front()).attachments;
    REQUIRE(current.size() == 3);
    CHECK(current[0].type == imza::Attachment::Type::TEXT);
    CHECK(current[0].content == "plain text");
    CHECK(current[1].type == imza::Attachment::Type::IMAGE);
    CHECK(current[1].media_type == "image/png");
    CHECK(current[1].content == image_bytes);
    CHECK(current[2].type == imza::Attachment::Type::PDF);
    CHECK(current[2].media_type == "application/pdf");
    CHECK(current[2].content == pdf_bytes);

    const std::filesystem::path legacy = home.path / "legacy-attachments.json";
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
#endif
}
