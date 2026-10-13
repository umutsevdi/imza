#include <doctest/doctest.h>

#include "conversation/session.h"
#include "turn/prompt.h"
#include "workspace/review.h"

#include "test_helpers.h"

namespace {

imza::RepositoryReview sample_review()
{
    const auto parsed
        = imza::parse_git_diff("diff --git a/src/foo.cpp b/src/foo.cpp\n"
                               "--- a/src/foo.cpp\n"
                               "+++ b/src/foo.cpp\n"
                               "@@ -1,2 +1,3 @@\n"
                               " same\n"
                               "-old\n"
                               "+new\n"
                               "+more\n");
    return std::get<imza::RepositoryReview>(parsed);
}

} // namespace

TEST_CASE("handoff prompt carries plan, revisions, messages, and diff")
{
    imza::Session session;
    session.begin_send("first ask");
    REQUIRE(session.create_plan(imza::test::PLAN_SKELETON).empty());
    REQUIRE(session.append_build_decision("chose A over B").empty());
    REQUIRE(session.create_plan(imza::test::PLAN_SKELETON + "\nmore").empty());

    const std::string prompt = imza::format_handoff_prompt(
        "INSTRUCTIONS", session.snapshot(), sample_review());
    CHECK(prompt.find("INSTRUCTIONS") == 0);
    CHECK(prompt.find("more") != std::string::npos);
    CHECK(prompt.find("chose A over B") != std::string::npos);
    CHECK(prompt.find("first ask") != std::string::npos);
    CHECK(prompt.find("src/foo.cpp") != std::string::npos);
}

TEST_CASE("handoff report replaces the previous one and leaves the plan alone")
{
    imza::Session session;
    session.begin_send("do the work");
    REQUIRE(session.create_plan(imza::test::PLAN_SKELETON).empty());
    REQUIRE(session.set_handoff_report("first report").empty());
    REQUIRE(session.set_handoff_report("second report").empty());

    const auto documents = session.documents();
    std::size_t handoffs = 0;
    for (const imza::SessionDoc& doc : *documents) {
        if (doc.kind == imza::SessionDoc::Kind::HANDOFF) {
            ++handoffs;
            CHECK(doc.content == "second report");
        }
    }
    CHECK(handoffs == 1);
    CHECK(session.plan_doc() == imza::test::PLAN_SKELETON);

    const std::vector<std::string> labels = imza::document_labels(*documents);
    REQUIRE(labels.size() == 2);
    CHECK(labels[0] == "Initial Plan");
    CHECK(labels[1] == "Handoff Report");
}
