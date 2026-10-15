#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "app/application_state.h"
#include "app/flows.h"
#include "conversation/persistence.h"
#include "conversation/session.h"
#include "conversation/session_store.h"
#include "platform/file_lock.h"
#include "test_fs.h"
#include "test_helpers.h"
#include "test_state.h"

namespace {

using DataHome = imza::test::IsolatedDataHome;
using imza::test::CurrentDirectory;

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
TEST_CASE("CLI opens and removes saved sessions by ID")
{
#ifdef _WIN32
    SKIP_ON_WIN32()
#endif
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
}

TEST_CASE("switch_session locks the target and reports foreign locks")
{
#ifdef _WIN32
    SKIP_ON_WIN32()
#endif
    DataHome home;
    CurrentDirectory directory;
    auto state = imza::test::make_test_state();
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

    imza::test::release_lock(foreign);
    imza::switch_session(*state, target_path);
    CHECK(state->session->title() == "Target");
    CHECK(state->session->error().empty());
    CHECK(state->session->session_id() == target_path.stem().string());

    state->session->begin_send("grew the target in place");
    REQUIRE(imza::save_session(*state->session) == imza::Status::OK);
    CHECK(imza::saved_sessions().size() == 2);
}
