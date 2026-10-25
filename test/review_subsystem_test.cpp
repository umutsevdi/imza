#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <string>

#include "app/flows.h"
#include "platform/command_runner.h"
#include "test_fs.h"
#include "test_state.h"
#include "turn/prompt_store.h"
#include "workspace/review_subsystem.h"

TEST_CASE("git diff parser builds files hunks and line numbers")
{
    const auto result
        = imza::parse_git_diff("diff --git a/file.cpp b/file.cpp\n"
                               "--- a/file.cpp\n"
                               "+++ b/file.cpp\n"
                               "@@ -9,2 +9,3 @@\n"
                               " same\n"
                               "-old\n"
                               "+new\n"
                               "+more\n");
    REQUIRE(std::holds_alternative<imza::RepositoryReview>(result));
    const auto& review = std::get<imza::RepositoryReview>(result);
    REQUIRE(review.files.size() == 1);
    CHECK(review.files[0].new_path == "file.cpp");
    CHECK(review.files[0].additions == 2);
    CHECK(review.files[0].deletions == 1);
    REQUIRE(review.files[0].hunks[0].lines.size() == 4);
    CHECK(review.files[0].hunks[0].old_start == 9);
    CHECK(review.files[0].hunks[0].old_count == 2);
    CHECK(review.files[0].hunks[0].new_start == 9);
    CHECK(review.files[0].hunks[0].new_count == 3);
    CHECK(review.files[0].hunks[0].lines[1].old_line == 10);
    CHECK(review.files[0].hunks[0].lines[2].new_line == 10);
}

TEST_CASE("git diff parser recognizes rename and binary files")
{
    const auto result
        = imza::parse_git_diff("diff --git a/old.png b/new.png\n"
                               "similarity index 100%\n"
                               "rename from old.png\n"
                               "rename to new.png\n"
                               "Binary files a/old.png and b/new.png differ\n");
    const auto& file = std::get<imza::RepositoryReview>(result).files[0];
    CHECK(file.old_path == "old.png");
    CHECK(file.new_path == "new.png");
    CHECK(file.kind == imza::ReviewFile::Kind::BINARY);
}

TEST_CASE("git diff parser defaults omitted hunk counts to one")
{
    const auto result
        = imza::parse_git_diff("diff --git a/file.cpp b/file.cpp\n"
                               "--- a/file.cpp\n"
                               "+++ b/file.cpp\n"
                               "@@ -8 +9 @@ function\n"
                               "-old\n"
                               "+new\n");
    const auto& hunk
        = std::get<imza::RepositoryReview>(result).files[0].hunks[0];
    CHECK(hunk.old_start == 8);
    CHECK(hunk.old_count == 1);
    CHECK(hunk.new_start == 9);
    CHECK(hunk.new_count == 1);
}

TEST_CASE("review state stores updates and removes comments")
{
    imza::ReviewSubsystem state;
    const auto id = state.add_comment({ "file.cpp", 2, 3, "line" }, "note");
    REQUIRE(state.snapshot().comments.size() == 1);
    state.update_comment(id, "updated");
    CHECK(state.snapshot().comments[0].body == "updated");
    state.request_jump(id);
    CHECK(state.snapshot().jump_comment == id);
    state.request_file_jump("other.cpp");
    CHECK(state.snapshot().jump_file == "other.cpp");
    state.clear_file_jump();
    CHECK_FALSE(state.snapshot().jump_file);
    state.delete_comment(id);
    CHECK(state.snapshot().comments.empty());
}

TEST_CASE("review comments format as a plan prompt and clear together")
{
    std::vector<imza::ReviewComment> comments {
        { 1, { "src/app.cpp", 10, 12, "line" }, "handle the error", false },
        { 2, { "include/app.h", 7, std::nullopt, "old" },
            "change the return type\nand update callers", true },
    };

    const imza::PromptStore prompts;
    CHECK(imza::format_review_plan_prompt(prompts.review_plan(), comments)
        == "Plan the changes needed to address the following review comments:"
           "\n\n- `src/app.cpp:12`\n  handle the error"
           "\n\n- `include/app.h:7` (stale)\n  change the return type"
           "\n  and update callers");

    imza::ReviewSubsystem state;
    state.add_comment(comments[0].anchor, comments[0].body);
    state.clear_comments();
    CHECK(state.snapshot().comments.empty());
    CHECK(imza::format_review_plan_prompt(prompts.review_plan(), { }).empty());
}

TEST_CASE("review state marks comments stale when their line disappears")
{
    imza::ReviewSubsystem state;
    state.add_comment({ "file.cpp", 2, 3, "line" }, "note");
    imza::RepositoryReview review;
    review.files.push_back(imza::ReviewFile { .old_path = "file.cpp",
        .new_path                                       = "file.cpp",
        .hunks                                          = { { "@@ -1 +1 @@",
            { { imza::ReviewLine::Kind::CONTEXT, 2, 3, "other" } } } } });
    state.set_result(std::move(review));
    CHECK(state.snapshot().comments[0].stale);
}

TEST_CASE("AI review prompt includes diff and existing comments")
{
    imza::RepositoryReview review;
    review.files.push_back(imza::ReviewFile { .old_path = "src/app.cpp",
        .new_path                                       = "src/app.cpp",
        .hunks                                          = { { "@@ -4 +4 @@",
            { { imza::ReviewLine::Kind::DELETION, 4, std::nullopt, "old" },
                { imza::ReviewLine::Kind::ADDITION, std::nullopt, 4,
                    "updated" } } } } });
    const std::vector<imza::ReviewComment> comments { { 1,
        { "src/app.cpp", std::nullopt, 4, "updated" }, "existing issue",
        false } };

    const imza::PromptStore prompts;
    const std::string prompt
        = imza::format_ai_review_prompt(prompts.review(), review, comments);
    CHECK(prompt.find("diff --git a/src/app.cpp b/src/app.cpp")
        != std::string::npos);
    CHECK(prompt.find("-old\\n+updated") != std::string::npos);
    CHECK(prompt.find("existing issue") != std::string::npos);
}

TEST_CASE("AI review response resolves changed-line anchors")
{
    imza::RepositoryReview review;
    review.files.push_back(imza::ReviewFile { .old_path = "src/app.cpp",
        .new_path                                       = "src/app.cpp",
        .hunks                                          = { { "@@ -8 +8 @@",
            { { imza::ReviewLine::Kind::DELETION, 8, std::nullopt, "old" },
                { imza::ReviewLine::Kind::ADDITION, std::nullopt, 8,
                    "updated" } } } } });

    const auto result = imza::parse_ai_review_response(
        "```json\n{\"findings\":[{\"file\":\"src/app.cpp\","
        "\"side\":\"new\",\"line\":8,\"severity\":\"P2\","
        "\"body\":\"The update loses the error.\"}]}\n```",
        review);

    REQUIRE(
        std::holds_alternative<std::vector<imza::ReviewCommentDraft>>(result));
    const auto& comments
        = std::get<std::vector<imza::ReviewCommentDraft>>(result);
    REQUIRE(comments.size() == 1);
    CHECK(comments[0].anchor.file == "src/app.cpp");
    CHECK(comments[0].anchor.new_line == 8);
    CHECK(comments[0].anchor.content == "updated");
    CHECK(comments[0].body == "[P2] The update loses the error.");
}

TEST_CASE("AI review rejects findings outside changed lines")
{
    imza::RepositoryReview review;
    review.files.push_back(imza::ReviewFile { .old_path = "src/app.cpp",
        .new_path                                       = "src/app.cpp",
        .hunks                                          = { { "@@ -1 +1 @@",
            { { imza::ReviewLine::Kind::CONTEXT, 1, 1, "same" } } } } });

    const auto result = imza::parse_ai_review_response(
        R"({"findings":[{"file":"src/app.cpp","side":"new","line":1,"severity":"P1","body":"Invalid anchor."}]})",
        review);

    REQUIRE(std::holds_alternative<std::string>(result));
}

TEST_CASE("review state adds AI comments without exact duplicates")
{
    imza::ReviewSubsystem state;
    const imza::ReviewLineAnchor anchor { "src/app.cpp", std::nullopt, 8,
        "updated" };
    state.add_comment(anchor, "The update loses the error.");

    const std::size_t added
        = state.add_comments({ { anchor, "[P2] The update loses the error." },
            { anchor, "[P3] A separate issue." } });

    CHECK(added == 1);
    const auto comments = state.comments();
    REQUIRE(comments.size() == 2);
    CHECK(comments[1].body == "[P3] A separate issue.");
}
TEST_CASE("ReviewSubsystem reloads the repository on its own worker")
{
    if (!imza::test::git_available()) {
        SKIP_TEST("git is not on PATH");
    }
    const imza::test::TempDir dir;
    const std::filesystem::path root = dir.path;
    const auto git                   = [&](const std::string& command) {
        return imza::command_ok(imza::run_command(
            "git " + command, std::chrono::seconds { 10 }, root));
    };
    REQUIRE(git("init -q"));
    imza::test::write_file(root / "a.txt", "one\n");
    REQUIRE(git("add a.txt"));
    // Disable signing: a global commit.gpgsign would otherwise make the
    // test depend on the developer's GPG setup.
    REQUIRE(git("-c user.email=t@t -c user.name=t -c commit.gpgsign=false "
                "commit -qm init"));
    imza::test::write_file(root / "a.txt", "one\ntwo\n");

    imza::ReviewSubsystem review;
    CHECK(review.snapshot().status == imza::ReviewSubsystem::LoadStatus::IDLE);

    review.set_active(true);
    review.reload(root);
    REQUIRE(imza::test::wait_until([&] {
        const auto current = review.snapshot();
        // Fail fast with the subsystem's own message instead of spinning
        // out: git failures (missing config, CRLF warnings, path quoting)
        // land here as ERROR.
        if (current.status == imza::ReviewSubsystem::LoadStatus::ERROR) {
            MESSAGE("review load error: " << current.error);
            return true;
        }
        return current.status == imza::ReviewSubsystem::LoadStatus::LOADED;
    }));
    const auto snapshot = review.snapshot();
    REQUIRE(snapshot.status == imza::ReviewSubsystem::LoadStatus::LOADED);
    REQUIRE(snapshot.review != nullptr);
    REQUIRE(snapshot.review->files.size() == 1);
    CHECK(snapshot.review->files[0].new_path == "a.txt");

    imza::test::write_file(root / "b.txt", "new\n");
    REQUIRE(git("add b.txt"));
    review.reload(root);
    REQUIRE(imza::test::wait_until([&] {
        const auto current = review.snapshot();
        return current.review != nullptr && current.review->files.size() == 2;
    }));
    review.stop();
}

TEST_CASE("ReviewSubsystem defers the repository load until it is visible")
{
    if (!imza::test::git_available()) {
        SKIP_TEST("git is not on PATH");
    }
    const imza::test::TempDir dir;
    const std::filesystem::path root = dir.path;
    const auto git                   = [&](const std::string& command) {
        return imza::command_ok(imza::run_command(
            "git " + command, std::chrono::seconds { 10 }, root));
    };
    REQUIRE(git("init -q"));
    imza::test::write_file(root / "a.txt", "one\n");
    REQUIRE(git("add a.txt"));
    REQUIRE(git("-c user.email=t@t -c user.name=t -c commit.gpgsign=false "
                "commit -qm init"));
    imza::test::write_file(root / "a.txt", "one\ntwo\n");

    imza::ReviewSubsystem review;
    CHECK_FALSE(review.active());

    review.reload(root);
    CHECK(review.snapshot().status == imza::ReviewSubsystem::LoadStatus::IDLE);

    review.set_active(true);
    REQUIRE(imza::test::wait_until([&] {
        const auto current = review.snapshot();
        if (current.status == imza::ReviewSubsystem::LoadStatus::ERROR) {
            MESSAGE("review load error: " << current.error);
            return true;
        }
        return current.status == imza::ReviewSubsystem::LoadStatus::LOADED;
    }));
    CHECK(
        review.snapshot().status == imza::ReviewSubsystem::LoadStatus::LOADED);
    CHECK(review.snapshot().review != nullptr);
    review.stop();
}

TEST_CASE("review pass retries a truncated reply instead of failing")
{
    auto pump  = std::make_shared<imza::test::PostPump>();
    auto calls = std::make_shared<std::atomic<int>>(0);
    imza::Config cfg;
    imza::Connection conn;
    conn.id       = "test";
    conn.endpoint = "https://example.test/v1/chat/completions";
    cfg.providers.push_back(conn);
    cfg.last_used = imza::LastUsed { "test", "m" };
    auto state    = imza::test::make_test_state(
        [pump](std::function<void()> task) { pump->fn()(std::move(task)); },
        std::move(cfg),
        [calls](const imza::ChatRequest&, const imza::StreamCallback& cb) {
            if (calls->fetch_add(1) == 0) {
                cb(imza::make_delta_event("{\"findings\":[{\"file\":\"a"));
                cb(imza::make_error_event(imza::Status::TRUNCATED, "cut off"));
                cb(imza::make_done_event());
                return imza::Status::OK;
            }
            cb(imza::make_delta_event(
                R"({"findings":[{"file":"file.cpp","side":"new","line":2,)"
                R"("severity":"P2","body":"real finding"}]})"));
            cb(imza::make_done_event());
            return imza::Status::OK;
        });
    imza::test::wait_for_environment(*state);

    state->review->set_result(
        imza::parse_git_diff("diff --git a/file.cpp b/file.cpp\n"
                             "--- a/file.cpp\n"
                             "+++ b/file.cpp\n"
                             "@@ -1,1 +1,2 @@\n"
                             " same\n"
                             "+added\n"));

    std::string error;
    (void)imza::run_review_pass(*state,
        imza::ReviewPassRequest { .pass = imza::ReviewPass::FINDINGS,
            .prompt                     = "review this",
            .label                      = "AI review",
            .complete = [state](std::string output) -> std::string {
                const auto review = state->review->snapshot().review;
                if (!review) {
                    return std::string { };
                }
                auto parsed = imza::parse_ai_review_response(output, *review);
                if (auto* parse_error = std::get_if<std::string>(&parsed)) {
                    return std::move(*parse_error);
                }
                state->review->add_comments(std::move(
                    std::get<std::vector<imza::ReviewCommentDraft>>(parsed)));
                return std::string { };
            } },
        error);

    REQUIRE(pump->wait_for([&] { return calls->load() >= 2; }));
    CHECK(state->session->error().find("invalid JSON") == std::string::npos);
    REQUIRE(state->review->comments().size() == 1);
    CHECK(state->review->comments().front().body.find("real finding")
        != std::string::npos);
}

TEST_CASE("an uncatalogued model sends no output cap on a review pass")
{
    // An uncatalogued model has an unknown context window, so the pass sends
    // no max_output_tokens: a fixed guess truncated valid replies.
    auto pump        = std::make_shared<imza::test::PostPump>();
    auto saw_cap     = std::make_shared<std::atomic<bool>>(false);
    auto saw_request = std::make_shared<std::atomic<bool>>(false);
    imza::Config cfg;
    imza::Connection conn;
    conn.id       = "test";
    conn.endpoint = "https://example.test/v1/chat/completions";
    cfg.providers.push_back(conn);
    cfg.last_used = imza::LastUsed { "test", "uncatalogued-model" };
    auto state    = imza::test::make_test_state(
        [pump](std::function<void()> task) { pump->fn()(std::move(task)); },
        std::move(cfg),
        [saw_cap, saw_request](
            const imza::ChatRequest& req, const imza::StreamCallback& cb) {
            saw_request->store(true);
            saw_cap->store(req.max_output_tokens.has_value());
            cb(imza::make_delta_event("{\"findings\":[]}"));
            cb(imza::make_done_event());
            return imza::Status::OK;
        });
    imza::test::wait_for_environment(*state);
    state->review->set_result(
        imza::parse_git_diff("diff --git a/file.cpp b/file.cpp\n"
                             "--- a/file.cpp\n"
                             "+++ b/file.cpp\n"
                             "@@ -1,1 +1,2 @@\n"
                             " same\n"
                             "+added\n"));

    std::string error;
    (void)imza::run_review_pass(*state,
        imza::ReviewPassRequest { .pass = imza::ReviewPass::FINDINGS,
            .prompt                     = "review this",
            .label                      = "AI review",
            .complete
            = [](std::string) -> std::string { return std::string { }; } },
        error);

    REQUIRE(pump->wait_for([&] { return saw_request->load(); }));
    CHECK_FALSE(saw_cap->load());
}

TEST_CASE("a truncated review reply surfaces as truncation, not invalid JSON")
{
    // Retry disabled: the truncation is reported as such, not parsed.
    auto pump = std::make_shared<imza::test::PostPump>();
    imza::Config cfg;
    imza::Connection conn;
    conn.id       = "test";
    conn.endpoint = "https://example.test/v1/chat/completions";
    cfg.providers.push_back(conn);
    cfg.last_used = imza::LastUsed { "test", "m" };
    auto state    = imza::test::make_test_state(
        [pump](std::function<void()> task) { pump->fn()(std::move(task)); },
        std::move(cfg),
        [](const imza::ChatRequest&, const imza::StreamCallback& cb) {
            cb(imza::make_delta_event("{\"findings\":[{\"file\":\"a"));
            cb(imza::make_error_event(imza::Status::TRUNCATED, "cut off"));
            cb(imza::make_done_event());
            return imza::Status::OK;
        });
    imza::test::wait_for_environment(*state);
    state->review->set_result(
        imza::parse_git_diff("diff --git a/file.cpp b/file.cpp\n"
                             "--- a/file.cpp\n"
                             "+++ b/file.cpp\n"
                             "@@ -1,1 +1,2 @@\n"
                             " same\n"
                             "+added\n"));

    std::string error;
    (void)imza::run_review_pass(*state,
        imza::ReviewPassRequest { .pass = imza::ReviewPass::FINDINGS,
            .prompt                     = "review this",
            .label                      = "AI review",
            .complete                   = [](std::string) -> std::string {
                return std::string("AI review returned invalid JSON.");
            },
            .retry = false },
        error);

    REQUIRE(pump->wait_for([&] { return !state->session->error().empty(); }));
    CHECK(state->session->error().find("truncated") != std::string::npos);
    CHECK(state->session->error().find("invalid JSON") == std::string::npos);
}

TEST_CASE("the two review passes run together but a repeat is refused")
{
    // Each pass owns its own slot, so AI Review and Handoff run at once; only
    // a second copy of the same pass is refused, and it says so.
    auto pump  = std::make_shared<imza::test::PostPump>();
    auto state = imza::test::make_test_state(
        [pump](std::function<void()> task) { pump->fn()(std::move(task)); },
        imza::test::review_pass_config(),
        [](const imza::ChatRequest&, const imza::StreamCallback& cb) {
            cb(imza::make_delta_event("{\"findings\":[]}"));
            cb(imza::make_done_event());
            return imza::Status::OK;
        });
    imza::test::wait_for_environment(*state);
    state->review->set_result(
        imza::parse_git_diff("diff --git a/file.cpp b/file.cpp\n"
                             "--- a/file.cpp\n"
                             "+++ b/file.cpp\n"
                             "@@ -1,1 +1,2 @@\n"
                             " same\n"
                             "+added\n"));

    state->review->set_pass_task(imza::ReviewPass::HANDOFF, 1);
    std::string error;
    const auto review = imza::run_review_pass(*state,
        imza::ReviewPassRequest { .pass = imza::ReviewPass::FINDINGS,
            .prompt                     = "review this",
            .label                      = "AI review",
            .complete
            = [](std::string) -> std::string { return std::string { }; } },
        error);
    CHECK(review.has_value());
    CHECK(error.empty());
    CHECK(state->review->snapshot().findings.task_id.has_value());
    CHECK(state->review->snapshot().handoff.task_id.has_value());

    imza::run_handoff(*state);
    CHECK(state->session->error().find("already running") != std::string::npos);
}
