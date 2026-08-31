#include <doctest/doctest.h>

#include "review.h"

TEST_CASE("git diff parser builds files hunks and line numbers")
{
    const auto result = ursa::parse_git_diff(
        "diff --git a/file.cpp b/file.cpp\n"
        "--- a/file.cpp\n"
        "+++ b/file.cpp\n"
        "@@ -9,2 +9,3 @@\n"
        " same\n"
        "-old\n"
        "+new\n"
        "+more\n");
    REQUIRE(std::holds_alternative<ursa::RepositoryReview>(result));
    const auto& review = std::get<ursa::RepositoryReview>(result);
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
    const auto result = ursa::parse_git_diff(
        "diff --git a/old.png b/new.png\n"
        "similarity index 100%\n"
        "rename from old.png\n"
        "rename to new.png\n"
        "Binary files a/old.png and b/new.png differ\n");
    const auto& file = std::get<ursa::RepositoryReview>(result).files[0];
    CHECK(file.old_path == "old.png");
    CHECK(file.new_path == "new.png");
    CHECK(file.kind == ursa::ReviewFile::Kind::BINARY);
}

TEST_CASE("git diff parser defaults omitted hunk counts to one")
{
    const auto result = ursa::parse_git_diff(
        "diff --git a/file.cpp b/file.cpp\n"
        "--- a/file.cpp\n"
        "+++ b/file.cpp\n"
        "@@ -8 +9 @@ function\n"
        "-old\n"
        "+new\n");
    const auto& hunk
        = std::get<ursa::RepositoryReview>(result).files[0].hunks[0];
    CHECK(hunk.old_start == 8);
    CHECK(hunk.old_count == 1);
    CHECK(hunk.new_start == 9);
    CHECK(hunk.new_count == 1);
}

TEST_CASE("review state stores updates and removes comments")
{
    ursa::ReviewState state;
    const auto id = state.add_comment({ "file.cpp", 2, 3, "line" }, "note");
    REQUIRE(state.snapshot().comments.size() == 1);
    state.update_comment(id, "updated");
    CHECK(state.snapshot().comments[0].body == "updated");
    state.request_jump(id);
    CHECK(state.snapshot().jump_comment == id);
    state.delete_comment(id);
    CHECK(state.snapshot().comments.empty());
}

TEST_CASE("review state marks comments stale when their line disappears")
{
    ursa::ReviewState state;
    state.add_comment({ "file.cpp", 2, 3, "line" }, "note");
    ursa::RepositoryReview review;
    review.files.push_back(ursa::ReviewFile { .old_path = "file.cpp",
        .new_path = "file.cpp",
        .hunks = { { "@@ -1 +1 @@",
            { { ursa::ReviewLine::Kind::CONTEXT, 2, 3, "other" } } } } });
    state.set_result(std::move(review));
    CHECK(state.snapshot().comments[0].stale);
}
