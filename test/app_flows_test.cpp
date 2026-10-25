#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <variant>
#include <vector>

#include <doctest/doctest.h>
#include <ftxui/component/event.hpp>
#include <ftxui/screen/screen.hpp>

#include "app/application_state.h"
#include "app/flows.h"
#include "common/util.h"
#include "conversation/persistence.h"
#include "conversation/session.h"
#include "conversation/session_store.h"
#include "test_fs.h"
#include "test_helpers.h"
#include "test_state.h"

#include <functional>

namespace {

using DataHome = imza::test::IsolatedDataHome;
using imza::test::CurrentDirectory;

} // namespace

TEST_CASE("permissions modal adds a grant from the keyboard")
{
    imza::test::IsolatedDataHome home;
    imza::test::AgentEnv env;
    imza::enqueue_user_modal(*env.state, imza::PermissionsModal { });
    REQUIRE(env.pump.wait_for([&] {
        return std::holds_alternative<imza::PermissionsModal>(
            env.session->modal());
    }));

    ftxui::Component modal = imza::make_modal(env.state);
    modal->Render();
    // Entering the add form moves the terminal cursor to the input: the
    // tab menu's focus requirement must not win over the focused chain.
    REQUIRE(modal->OnEvent(ftxui::Event::ArrowDown));
    modal->Render();
    ftxui::Screen screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(80), ftxui::Dimension::Fixed(30));
    ftxui::Render(screen, modal->Render());
    CHECK(screen.cursor().shape != ftxui::Screen::Cursor::Shape::Hidden);

    // A missing directory is refused inline without writing the config.
    REQUIRE(modal->OnEvent(ftxui::Event::Character("d")));
    REQUIRE(modal->OnEvent(ftxui::Event::Return));
    modal->Render();
    const std::string refused = imza::test::without_ansi(
        imza::test::to_text(modal->Render(), 80, 30));
    CHECK(refused.find("Not a directory") != std::string::npos);
    CHECK(env.state->config->snapshot()->allow.directories.empty());

    // The commands tab accepts a grantable command and installs it live.
    REQUIRE(modal->OnEvent(ftxui::Event::Tab));
    REQUIRE(modal->OnEvent(ftxui::Event::Character("r")));
    REQUIRE(modal->OnEvent(ftxui::Event::Character("g")));
    REQUIRE(modal->OnEvent(ftxui::Event::Return));
    REQUIRE(env.pump.wait_for([&] {
        return imza::grants_cover(env.state->permissions->snapshot(),
            imza::ShellCommandGrant { "rg" });
    }));
    imza::close_modal(*env.state);
}

TEST_CASE("skills modal marks bundled skills and skips their policies")
{
    // The bundled root must exist before the environment scan runs.
    imza::test::TempDir bundled_root;
    imza::test::write_file(bundled_root.path / "imza-docs" / "SKILL.md",
        "---\ndescription: bundled manual\n---\n\nDocs body.\n");
    const imza::test::ScopedEnv bundled(
        "IMZA_BUNDLED_SKILLS", bundled_root.path.string());

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
}

TEST_CASE("CLI session list aligns columns without terminal tabs")
{
    const std::vector<imza::SavedSession> sessions {
        { "1789291986542-c06df75e.json", "Long title", "2026-09-13 12:33:06" },
        { "short.json", "Other title", "2026-09-13 12:25:53" },
    };

    const std::string listed = imza::format_session_list(sessions);
    CHECK(listed.find('\t') == std::string::npos);
    const std::vector<std::string> lines = imza::split_lines(listed);
    REQUIRE(lines.size() == 3);
    // Every row puts the title at the same column, past the id and date.
    const std::size_t title_at = lines.front().find("TITLE");
    REQUIRE(title_at != std::string::npos);
    CHECK(lines[1].find("Long title") == title_at);
    CHECK(lines[2].find("Other title") == title_at);
    // A short id still leaves the saved column aligned.
    CHECK(lines[1].find("2026-09-13 12:33:06")
        == lines[2].find("2026-09-13 12:25:53"));
}
TEST_CASE("CLI opens and removes saved sessions by ID")
{
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

    imza::test::ForeignLock foreign(target_path);

    imza::switch_session(*state, target_path);
    CHECK(state->session->title() == "Current");
    CHECK(
        state->session->error() == "Session is open in another imza process.");
    CHECK(state->session->session_id() == current_stem);
    CHECK(imza::saved_sessions().size() == 2);

    foreign.release();
    imza::switch_session(*state, target_path);
    CHECK(state->session->title() == "Target");
    CHECK(state->session->error().empty());
    CHECK(state->session->session_id() == target_path.stem().string());

    state->session->begin_send("grew the target in place");
    REQUIRE(imza::save_session(*state->session) == imza::Status::OK);
    CHECK(imza::saved_sessions().size() == 2);
}
TEST_CASE("new session clears every conversation-scoped surface")
{
    imza::test::IsolatedDataHome home;
    auto state = imza::test::make_test_state();
    imza::test::wait_for_environment(*state);

    const std::filesystem::path skill_path
        = imza::test::TempDir { }.file("SKILL.md");
    imza::test::write_file(skill_path, "instructions");
    state->session->record_skill_load(skill_path, "body");
    state->session->set_pending_skill_turn(
        imza::PendingSkillTurn { "text", { }, { }, 0 });
    state->session->queue.enqueue(
        imza::ViewerModal { .title = "t", .content = "c" },
        imza::ModalOrigin::USER);
    REQUIRE(state->permissions->install(
        { imza::ExternalGrant { std::filesystem::temp_directory_path() } }));

    imza::run_slash(*state, "/new");

    CHECK(state->session->loaded_skill_directories().empty());
    CHECK_FALSE(state->session->pending_skill_turn().has_value());
    CHECK(state->session->queue.size() == 0);
    CHECK(state->permissions->snapshot()->empty());
    CHECK(state->session->has_items() == false);
}

namespace {

const char* review_diff()
{
    return "diff --git a/file.cpp b/file.cpp\n"
           "--- a/file.cpp\n+++ b/file.cpp\n@@ -1,1 +1,2 @@\n same\n+added\n";
}

std::string findings_reply()
{
    return "{\"findings\":[{\"file\":\"file.cpp\",\"side\":\"new\","
           "\"line\":2,\"severity\":\"P1\",\"body\":\"oops\"}]}";
}

std::string toast_text(const imza::ApplicationState& state)
{
    std::string text;
    for (const imza::Notification& entry :
        *state.notifications->notifications()) {
        if (!text.empty()) {
            text += "\n";
        }
        text += entry.description;
    }
    return text;
}

} // namespace

TEST_CASE("a finished review pass alerts and toasts the attended root")
{
    auto pump  = std::make_shared<imza::test::PostPump>();
    auto state = imza::test::make_test_state(
        [pump](std::function<void()> task) { pump->fn()(std::move(task)); },
        imza::test::review_pass_config(),
        [](const imza::ChatRequest&, const imza::StreamCallback& cb) {
            cb(imza::make_delta_event(findings_reply()));
            cb(imza::make_done_event());
            return imza::Status::OK;
        });
    imza::test::wait_for_environment(*state);
    state->review->set_result(imza::parse_git_diff(review_diff()));
    std::vector<std::string> alerts;
    auto alerts_subscription = state->notifications->subscribe_to_alert(
        [&alerts](const std::string& message) { alerts.push_back(message); });

    std::string error;
    (void)imza::run_review_pass(*state,
        imza::ReviewPassRequest { .pass = imza::ReviewPass::FINDINGS,
            .prompt                     = "review this",
            .label                      = "AI review",
            .complete =
                [state](std::string output) {
                    const imza::ReviewSubsystem::Snapshot current
                        = state->review->snapshot();
                    if (!current.review) {
                        return std::string { };
                    }
                    imza::AiReviewParseResult parsed
                        = imza::parse_ai_review_response(
                            output, *current.review);
                    if (auto* parse_error = std::get_if<std::string>(&parsed)) {
                        return std::move(*parse_error);
                    }
                    std::vector<imza::ReviewCommentDraft> drafts = std::move(
                        std::get<std::vector<imza::ReviewCommentDraft>>(
                            parsed));
                    const std::size_t count = drafts.size();
                    state->review->add_comments(std::move(drafts));
                    if (state->notifications) {
                        state->notifications->publish(false,
                            count == 1 ? "AI review found 1 finding."
                                       : "AI review found "
                                    + std::to_string(count) + " findings.");
                    }
                    return std::string { };
                } },
        error);
    CHECK(error.empty());
    REQUIRE(pump->wait_for([&] { return alerts.size() == 1; }));
    CHECK(alerts.front() == "AI review finished");
    CHECK(toast_text(*state) == "AI review found 1 finding.");
    CHECK(state->session->error().empty());

    imza::run_handoff(*state);
    REQUIRE(pump->wait_for([&] { return alerts.size() == 2; }));
    CHECK(alerts.back() == "Handoff report ready");
    REQUIRE(pump->wait_for(
        [&] { return !state->session->snapshot().documents.empty(); }));
    CHECK(
        toast_text(*state).find("Handoff report saved.") != std::string::npos);
}

TEST_CASE("a cancelled review pass stays silent")
{
    auto pump  = std::make_shared<imza::test::PostPump>();
    auto state = imza::test::make_test_state(
        [pump](std::function<void()> task) { pump->fn()(std::move(task)); },
        imza::test::review_pass_config(),
        [](const imza::ChatRequest&, const imza::StreamCallback& cb) {
            cb(imza::make_delta_event(findings_reply()));
            return imza::Status::CANCELLED;
        });
    imza::test::wait_for_environment(*state);
    state->review->set_result(imza::parse_git_diff(review_diff()));
    std::vector<std::string> alerts;
    auto alerts_subscription = state->notifications->subscribe_to_alert(
        [&alerts](const std::string& message) { alerts.push_back(message); });

    std::string error;
    (void)imza::run_review_pass(*state,
        imza::ReviewPassRequest { .pass = imza::ReviewPass::FINDINGS,
            .prompt                     = "review this",
            .label                      = "AI review",
            .complete
            = [](std::string) -> std::string { return std::string { }; },
            .retry = false },
        error);

    REQUIRE(pump->wait_for([&] {
        return !state->review->snapshot().findings.task_id.has_value();
    }));
    CHECK(alerts.empty());
    CHECK(state->session->error().empty());
    CHECK(toast_text(*state).empty());
}
