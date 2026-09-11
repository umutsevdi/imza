#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#include <doctest/doctest.h>

#include "app/flows.h"
#include "conversation/persistence.h"
#include "conversation/session.h"

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

TEST_CASE("saved sessions are immutable and fork on a new prompt")
{
#ifdef _WIN32
    return;
#else
    DataHome home;
    imza::Session source;
    source.set_title("Saved title");
    source.begin_send("hello");
    source.append_assistant("model", "off");
    source.apply(imza::make_delta_event("world"), { });

    REQUIRE(imza::save_session(source) == imza::Status::OK);
    CHECK_FALSE(source.snapshot_for_save());
    auto saved = imza::saved_sessions();
    REQUIRE(saved.size() == 1);
    CHECK(saved.front().path.parent_path() == imza::sessions_dir());
    CHECK(imza::sessions_dir() == imza::data_dir() / "sessions");
    CHECK(saved.front().title == "Saved title");
    const std::filesystem::path saved_path = saved.front().path;

    source.begin_send("follow-up");
    CHECK(source.snapshot_for_save().has_value());
    REQUIRE(imza::save_session(source) == imza::Status::OK);
    saved = imza::saved_sessions();
    REQUIRE(saved.size() == 2);
    CHECK(std::any_of(saved.begin(), saved.end(),
        [&](const auto& entry) { return entry.path == saved_path; }));

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
    CHECK(std::filesystem::current_path() == other);
    CHECK(loaded.title() == "Saved title");
    REQUIRE(loaded.items().size() == 2);
    CHECK(std::get<imza::UserTurn>(loaded.items()[0]).text == "hello");
    CHECK(std::get<imza::AssistantTurn>(loaded.items()[1]).markdown == "world");

    loaded.begin_send("parallel continuation");
    REQUIRE(imza::save_session(loaded) == imza::Status::OK);
    saved = imza::saved_sessions();
    REQUIRE(saved.size() == 3);

    REQUIRE(imza::save_session(loaded) == imza::Status::OK);
    CHECK(imza::saved_sessions().size() == 3);
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
    const bool escaped  = bytes.find("Eyl\\u00fcl") != std::string::npos;
    const bool utf8     = bytes.find("Eyl\xc3\xbcl") != std::string::npos;
    const bool encoded  = escaped || utf8;
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
             << "  \"workspace\": \"" << directory.original.string() << "/gone-away\",\n"
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
