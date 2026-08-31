#include <algorithm>
#include <cstdlib>
#include <filesystem>

#include <doctest/doctest.h>

#include "environment/environment.h"
#include "agent/subsystems/session.h"
#include "provider/pricing.h"
#include "agent/subsystems/session_store.h"

namespace {

struct DataHome {
    std::filesystem::path path
        = std::filesystem::temp_directory_path() / "ursa_session_store_test";
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

    ~CurrentDirectory() { ursa::get_environment()->chdir(original); }
};

} // namespace

TEST_CASE("saved sessions are immutable and fork on a new prompt")
{
#ifdef _WIN32
    return;
#else
    DataHome home;
    ursa::Session source;
    source.set_title("Saved title");
    source.begin_send("hello");
    source.append_assistant("model", "off");
    source.apply(ursa::make_delta_event("world"), { });

    REQUIRE(ursa::save_session(source) == ursa::Status::OK);
    auto saved = ursa::saved_sessions();
    REQUIRE(saved.size() == 1);
    CHECK(saved.front().path.parent_path() == ursa::sessions_dir());
    CHECK(ursa::sessions_dir() == ursa::data_dir() / "sessions");
    CHECK(saved.front().title == "Saved title");
    const std::filesystem::path saved_path = saved.front().path;

    source.begin_send("follow-up");
    REQUIRE(ursa::save_session(source) == ursa::Status::OK);
    saved = ursa::saved_sessions();
    REQUIRE(saved.size() == 2);
    CHECK(std::any_of(saved.begin(), saved.end(),
        [&](const auto& entry) { return entry.path == saved_path; }));

    CurrentDirectory directory;
    const auto other = home.path / "other-workspace";
    std::filesystem::create_directories(other);
    REQUIRE(ursa::get_environment()->chdir(other));

    ursa::Session loaded;
    std::filesystem::path workspace;
    REQUIRE(ursa::load_session(saved_path, loaded, &workspace)
        == ursa::Status::OK);
    CHECK(workspace == directory.original);
    CHECK(std::filesystem::current_path() == other);
    CHECK(loaded.title() == "Saved title");
    REQUIRE(loaded.items().size() == 2);
    CHECK(std::get<ursa::UserTurn>(loaded.items()[0]).text == "hello");
    CHECK(std::get<ursa::AssistantTurn>(loaded.items()[1]).markdown == "world");

    loaded.begin_send("parallel continuation");
    REQUIRE(ursa::save_session(loaded) == ursa::Status::OK);
    saved = ursa::saved_sessions();
    REQUIRE(saved.size() == 3);

    REQUIRE(ursa::save_session(loaded) == ursa::Status::OK);
    CHECK(ursa::saved_sessions().size() == 3);
#endif
}

TEST_CASE("empty sessions are not saved")
{
#ifdef _WIN32
    return;
#else
    DataHome home;
    ursa::Session session;
    CHECK(ursa::save_session(session) == ursa::Status::OK);
    CHECK(ursa::saved_sessions().empty());
#endif
}

TEST_CASE("saved sessions retain delegated-agent chat transcripts")
{
#ifdef _WIN32
    return;
#else
    DataHome home;
    ursa::Session source;
    source.begin_send("delegate");
    source.append_assistant("model", "off");
    const ursa::ToolCallRequest request { "subagent", "{}", "", "call-1" };
    source.append_tool(request);
    source.set_tool_subagent_chats(
        request, { { "Agent 1 (research)", "## Assistant\n\nreport" } });
    source.fill_tool_result(
        request, { ursa::ToolCall::Result::Kind::OUTPUT, "report" });
    source.finish_session("");

    REQUIRE(ursa::save_session(source) == ursa::Status::OK);
    const auto saved = ursa::saved_sessions();
    REQUIRE(saved.size() == 1);
    ursa::Session loaded;
    REQUIRE(ursa::load_session(saved.front().path, loaded) == ursa::Status::OK);
    REQUIRE(loaded.items().size() == 3);
    const auto& call = std::get<ursa::ToolCall>(loaded.items()[2]);
    REQUIRE(call.subagent_chats.size() == 1);
    CHECK(call.subagent_chats[0].title == "Agent 1 (research)");
    CHECK(
        call.subagent_chats[0].transcript.find("report") != std::string::npos);
#endif
}
