#include <string>

#include <doctest/doctest.h>

#include "format.h"

TEST_CASE("question_form_markdown renders prompt and options")
{
    ursa::QuestionForm form { { "model?", { "gpt-4o", "claude" }, false,
        false } };
    const std::string md = ursa::question_form_markdown(form);
    CHECK(md.find("Question: \"model?\"") != std::string::npos);
    CHECK(md.find("- gpt-4o") != std::string::npos);
    CHECK(md.find("- claude") != std::string::npos);
}

TEST_CASE("modal_answer_markdown renders selected then free text")
{
    ursa::ModalAnswer ans { { { { "Option 3" }, "extra note" } } };
    const std::string md = ursa::modal_answer_markdown(ans);
    CHECK(md.find("User answered:") == 0);
    CHECK(md.find("> Option 3\n> extra note") != std::string::npos);
}

TEST_CASE("modal_answer_markdown renders Q/A pairs with prompt")
{
    ursa::ModalAnswer ans;
    ans.cards.push_back(
        ursa::QuestionAnswer { { "PostgreSQL" }, "", "storage backend?" });
    ans.cards.push_back(
        ursa::QuestionAnswer { { "Auth", "Billing" }, "", "features?" });
    ans.cards.push_back(
        ursa::QuestionAnswer { {}, "my own region", "region?" });
    ans.cards.push_back(
        ursa::QuestionAnswer { {}, "", "anything else?" });
    const std::string md = ursa::modal_answer_markdown(ans);
    CHECK(md.find("**storage backend?**") != std::string::npos);
    CHECK(md.find("PostgreSQL") != std::string::npos);
    CHECK(md.find("Auth, Billing") != std::string::npos);
    CHECK(md.find("my own region") != std::string::npos);
    CHECK(md.find("anything else?**") != std::string::npos);
    CHECK(md.find("—") != std::string::npos);
}
