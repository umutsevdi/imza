#include <cstdlib>
#include <filesystem>

#include <doctest/doctest.h>

#include "session.h"
#include "session_store.h"
#include "environment.h"

namespace {

struct DataHome {
    std::filesystem::path path
        = std::filesystem::temp_directory_path() / "ursa_session_store_test";
    std::string previous;
    bool had_previous = false;

    DataHome()
    {
        if (const char* value = std::getenv("XDG_DATA_HOME")) {
            previous = value;
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
        ursa::get_environment()->chdir(original);
    }
};

}

TEST_CASE("sessions save, list and load")
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
    CHECK(saved.front().title == "Saved title");
    const std::filesystem::path saved_path = saved.front().path;

    source.begin_send("follow-up");
    REQUIRE(ursa::save_session(source) == ursa::Status::OK);
    saved = ursa::saved_sessions();
    REQUIRE(saved.size() == 1);
    CHECK(saved.front().path == saved_path);

    CurrentDirectory directory;
    const auto other = home.path / "other-workspace";
    std::filesystem::create_directories(other);
    REQUIRE(ursa::get_environment()->chdir(other));

    ursa::Session loaded;
    REQUIRE(ursa::load_session(saved_path, loaded) == ursa::Status::OK);
    CHECK(std::filesystem::current_path() == directory.original);
    CHECK(loaded.title() == "Saved title");
    REQUIRE(loaded.items().size() == 3);
    CHECK(std::get<ursa::UserTurn>(loaded.items()[0]).text == "hello");
    CHECK(std::get<ursa::AssistantTurn>(loaded.items()[1]).markdown == "world");
    CHECK(std::get<ursa::UserTurn>(loaded.items()[2]).text == "follow-up");

    loaded.append_assistant();
    REQUIRE(ursa::save_session(loaded) == ursa::Status::OK);
    saved = ursa::saved_sessions();
    REQUIRE(saved.size() == 1);
    CHECK(saved.front().path == saved_path);
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
