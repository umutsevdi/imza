#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "app/application_state.h"
#include "app/flows.h"
#include "common/util.h"
#include "conversation/persistence.h"
#include "conversation/session.h"
#include "conversation/session_store.h"
#include "platform/file_lock.h"
#include "test_fs.h"
#include "test_helpers.h"
#include "test_state.h"

#include <functional>

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

TEST_CASE("markdown output renders wrapped styled text at a fixed width")
{
    const std::string text = imza::test::without_ansi(
        imza::render_markdown_to_text("# Heading\\n\\nBody text.\\n", 40));
    CHECK(text.find("Heading") != std::string::npos);
    CHECK(text.find("Body text.") != std::string::npos);
    for (const std::string& line :
        imza::split_lines(imza::render_markdown_to_text(
            "A paragraph that is longer than the requested wrap width "
            "so it must fold.\\n",
            30))) {
        CHECK(line.size() <= 30);
    }
}

TEST_CASE("update flow runs work and returns its exit code")
{
    const int code = imza::run_update_display(
        [](const std::function<void(std::string)>& progress) {
            progress("stage one");
            return 7;
        });
    CHECK(code == 7);
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
TEST_CASE("new session clears every conversation-scoped surface")
{
    imza::test::IsolatedDataHome home;
    auto state = imza::test::make_test_state();
    imza::test::wait_for_environment(*state);

    // Conversation-scoped state: a loaded skill, a queued modal, a pending
    // skill turn, and a session grant.
    const std::filesystem::path skill_path
        = imza::test::TempDir { }.file("SKILL.md");
    imza::test::write_file(skill_path, "instructions");
    state->session->record_skill_load(skill_path, "body");
    state->session->set_pending_skill_turn(
        imza::PendingSkillTurn { "text", { }, { }, 0 });
    state->session->queue.enqueue(
        imza::ViewerModal { .title = "t", .content = "c" },
        imza::ModalOrigin::USER);
    REQUIRE(state->permissions->install({ imza::ExternalGrant { "/tmp" } }));

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

// test_config() carries no endpoint, so the pass workers would refuse
// with a configuration error before reaching the stream stub.
imza::Config review_pass_config()
{
    imza::Config cfg;
    imza::Connection conn;
    conn.id       = "test";
    conn.endpoint = "https://example.test/v1/chat/completions";
    cfg.providers.push_back(conn);
    cfg.last_used = imza::LastUsed { "test", "m" };
    return cfg;
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
        review_pass_config(),
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

TEST_CASE("a failed review pass rings without adding a second toast")
{
    // Truncation with retry disabled: set_error already publishes the
    // error toast, so the pass failure must only add the bell.
    auto pump  = std::make_shared<imza::test::PostPump>();
    auto state = imza::test::make_test_state(
        [pump](std::function<void()> task) { pump->fn()(std::move(task)); },
        review_pass_config(),
        [](const imza::ChatRequest&, const imza::StreamCallback& cb) {
            cb(imza::make_delta_event("{\"findings\":[{\"file\":\"a"));
            cb(imza::make_error_event(imza::Status::TRUNCATED, "cut off"));
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
            .complete
            = [](std::string) -> std::string { return std::string { }; },
            .retry = false },
        error);

    REQUIRE(pump->wait_for([&] { return !state->session->error().empty(); }));
    CHECK(state->session->error().find("truncated") != std::string::npos);
    REQUIRE(pump->wait_for([&] { return alerts.size() == 1; }));
    CHECK(alerts.front() == "AI review failed");
    // Exactly one error toast, from set_error itself.
    const auto entries = state->notifications->notifications();
    REQUIRE(entries->size() == 1);
    CHECK(entries->front().error);
}

TEST_CASE("a cancelled review pass stays silent")
{
    auto pump  = std::make_shared<imza::test::PostPump>();
    auto state = imza::test::make_test_state(
        [pump](std::function<void()> task) { pump->fn()(std::move(task)); },
        review_pass_config(),
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
