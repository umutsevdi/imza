#include <doctest/doctest.h>

#include "workspace/review.h"

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
    imza::ReviewState state;
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

    CHECK(imza::format_review_plan_prompt(comments)
        == "Plan the changes needed to address the following review comments:"
           "\n\n- `src/app.cpp:12`\n  handle the error"
           "\n\n- `include/app.h:7` (stale)\n  change the return type"
           "\n  and update callers");

    imza::ReviewState state;
    state.add_comment(comments[0].anchor, comments[0].body);
    state.clear_comments();
    CHECK(state.snapshot().comments.empty());
    CHECK(imza::format_review_plan_prompt({ }).empty());
}

TEST_CASE("review state marks comments stale when their line disappears")
{
    imza::ReviewState state;
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

    const std::string prompt = imza::format_ai_review_prompt(review, comments);
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
    imza::ReviewState state;
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
