#include <string>

#include <doctest/doctest.h>

#include "test_helpers.h"
#include "test_state.h"
#include "ui/ui.h"

namespace {

imza::LayoutCtx wide_layout()
{
    return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 100, 40 };
}

} // namespace

TEST_CASE("plan tab renders the chat")
{
    auto state = imza::test::make_test_state();
    state->session->begin_send("plan this");

    auto chat = imza::make_chat(state, [] {
        return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 100, 40 };
    });
    auto plan = imza::make_plan_tab(state, [] { return wide_layout(); }, chat);
    plan->Add(chat);

    const std::string rendered = imza::test::to_text(plan->Render(), 100, 40);
    CHECK(rendered.find("plan this") != std::string::npos);
}

TEST_CASE("build tab renders the chat")
{
    auto state = imza::test::make_test_state();
    state->session->begin_send("build this");

    imza::SidechatStatus status;
    auto chat     = imza::make_chat(state, [] {
        return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 100, 40 };
    });
    auto sidechat = imza::make_sidechat_component(state, [] { }, status);

    auto build = imza::make_build_tab(
        state, [] { return wide_layout(); }, chat, sidechat, status);
    build->Add(chat);

    const std::string rendered = imza::test::to_text(build->Render(), 100, 40);
    CHECK(rendered.find("build this") != std::string::npos);
}

TEST_CASE("review tab renders its pane")
{
    auto state  = imza::test::make_test_state();
    auto review = imza::make_review(
        state, [] { return wide_layout(); }, [](imza::WorkflowPhase) { });

    const std::string rendered = imza::test::to_text(review->Render(), 100, 40);
    CHECK_FALSE(rendered.empty());
}
TEST_CASE("chat hints resolve the phase line provider per render")
{
    imza::ChatHints hints;
    int calls           = 0;
    hints.phase_line_fn = [&calls] {
        ++calls;
        return calls == 1 ? std::string("no toggle here")
                          : std::string("Ctrl+S Sidechat");
    };

    auto state = imza::test::make_test_state();
    auto chat  = imza::make_chat(
        state,
        [] { return imza::LayoutCtx { imza::LayoutCtx::Kind::WIDE, 100, 40 }; },
        hints);

    const std::string first = imza::test::to_text(chat->Render(), 100, 40);
    CHECK(first.find("no toggle here") != std::string::npos);
    CHECK(first.find("Ctrl+S Sidechat") == std::string::npos);

    const std::string second = imza::test::to_text(chat->Render(), 100, 40);
    CHECK(second.find("Ctrl+S Sidechat") != std::string::npos);
}
