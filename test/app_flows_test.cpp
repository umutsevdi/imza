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

TEST_CASE("skills modal marks bundled skills and skips their policies")
{
    // The bundled root must exist before the environment scan runs.
    imza::test::TempDir bundled_root;
    imza::test::write_file(bundled_root.path / "imza-docs" / "SKILL.md",
        "---\ndescription: bundled manual\n---\n\nDocs body.\n");
    ::setenv("IMZA_BUNDLED_SKILLS", bundled_root.path.string().c_str(), 1);

    DataHome home;
    auto state = imza::test::make_test_state();
    imza::test::wait_for_environment(*state);

    const imza::SkillsModal modal = imza::skills_modal(*state);
    bool saw_bundled              = false;
    for (const auto& entry : modal.entries) {
        if (entry.name == "imza-docs") {
            saw_bundled = true;
            REQUIRE(entry.bundled);
            CHECK(entry.policy == imza::SkillPolicy::ALLOW);
        } else {
            CHECK_FALSE(entry.bundled);
        }
    }
    REQUIRE(saw_bundled);

    // Simulating the modal's persist: bundled entries are excluded, and a
    // deny policy set for another skill never lands on the bundled one.
    imza::SkillPolicyChanges changes;
    for (const auto& entry : modal.entries) {
        if (!entry.bundled) {
            changes.entries.push_back(
                { entry.name, entry.project_root, entry.policy });
        }
    }
    CHECK(state->providers->set_skill_policies(changes));
    const imza::Config config = state->providers->config();
    CHECK(config.global_skills.find("imza-docs") == config.global_skills.end());
    ::unsetenv("IMZA_BUNDLED_SKILLS");
}

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
