#include "app/application_state.h"
#include "app/flows.h"
#include "common/modal.h"
#include "conversation/session.h"
#include "platform/config.h"
#include "test_helpers.h"
#include "test_state.h"
#include "ui/ui.h"

#include <chrono>
#include <string>
#include <vector>

#include <doctest/doctest.h>
#include <ftxui/component/animation.hpp>

namespace imza {

TEST_CASE("sidechat status reports no modal when the pane is open without one")
{
    auto state = imza::test::make_test_state();
    SidechatStatus status;
    auto component = make_sidechat_component(state, [] { }, status);

    REQUIRE_FALSE(state->sidechat_open);
    CHECK_FALSE(status.has_modal());

    open_sidechat(*state);
    component->Render();
    REQUIRE(state->sidechat_open);
    CHECK_FALSE(status.has_modal());

    enqueue_user_modal(
        *state->sidechat, ConnectModal { ConnectModal::Entry::MANAGE });
    CHECK(status.has_modal());

    close_sidechat(*state);
    component->Render();
    CHECK_FALSE(status.has_modal());
}

TEST_CASE("sidechat toggle hides and reopens without discarding the session")
{
    auto state = imza::test::make_test_state();
    REQUIRE_FALSE(state->sidechat_open);
    REQUIRE(state->sidechat == nullptr);

    open_sidechat(*state);
    REQUIRE(state->sidechat_open);
    REQUIRE(state->sidechat != nullptr);
    std::shared_ptr<ApplicationState> sidechat = state->sidechat;
    CHECK_FALSE(sidechat->sidechat_context_seeded);
    CHECK(sidechat->session->snapshot().compacted_summary.empty());

    sidechat->session->append_item(UserTurn { "sidechat question", { } });
    enqueue_user_modal(*sidechat, ConnectModal { ConnectModal::Entry::MANAGE });
    REQUIRE(sidechat->session->items().size() == 1);
    CHECK(std::holds_alternative<ConnectModal>(sidechat->session->modal()));

    close_sidechat(*state);
    CHECK_FALSE(state->sidechat_open);
    REQUIRE(state->sidechat == sidechat);
    const SessionSnapshot hidden = sidechat->session->snapshot();
    CHECK(hidden.items.size() == 1);
    CHECK(std::holds_alternative<ConnectModal>(sidechat->session->modal()));

    open_sidechat(*state);
    CHECK(state->sidechat_open);
    REQUIRE(state->sidechat == sidechat);
    const SessionSnapshot reshown = sidechat->session->snapshot();
    CHECK(reshown.items.size() == 1);
    CHECK(std::holds_alternative<ConnectModal>(sidechat->session->modal()));
}

TEST_CASE("sidechat loads its context lazily on the first prompt")
{
    auto state = imza::test::make_test_state();
    state->session->append_item(UserTurn { "first question", { } });
    state->session->append_item(
        AssistantTurn { "first answer", "", "", { }, "", "" });
    open_sidechat(*state);
    REQUIRE(state->sidechat_open);

    const SessionSnapshot empty = state->sidechat->session->snapshot();
    CHECK_FALSE(state->sidechat->sidechat_context_seeded);
    CHECK(empty.compacted_summary.empty());
    CHECK(empty.items.empty());

    ensure_sidechat_seeded(*state->sidechat);
    REQUIRE(state->sidechat->sidechat_context_seeded);
    const SessionSnapshot seeded = state->sidechat->session->snapshot();
    CHECK(seeded.compacted_summary.find("first answer") != std::string::npos);
    CHECK(seeded.items.empty());

    ensure_sidechat_seeded(*state->sidechat);
    CHECK(state->sidechat->session->snapshot().compacted_summary
        == seeded.compacted_summary);
}

TEST_CASE("refresh_sidechat clears the sidechat and the next prompt reloads")
{
    auto state = imza::test::make_test_state();
    state->session->append_item(UserTurn { "first question", { } });
    state->session->append_item(
        AssistantTurn { "first answer", "", "", { }, "", "" });
    open_sidechat(*state);
    ensure_sidechat_seeded(*state->sidechat);
    REQUIRE(state->sidechat->sidechat_context_seeded);

    state->session->append_item(UserTurn { "second question", { } });
    state->session->append_item(
        AssistantTurn { "second answer", "", "", { }, "", "" });
    state->sidechat->session->append_item(
        UserTurn { "sidechat question", { } });

    refresh_sidechat(*state);

    const SessionSnapshot shell = state->sidechat->session->snapshot();
    CHECK_FALSE(state->sidechat->sidechat_context_seeded);
    CHECK(shell.compacted_summary.empty());
    CHECK(shell.items.empty());

    ensure_sidechat_seeded(*state->sidechat);
    REQUIRE(state->sidechat->sidechat_context_seeded);
    const SessionSnapshot fresh = state->sidechat->session->snapshot();
    CHECK(fresh.compacted_summary.find("second answer") != std::string::npos);
    CHECK(
        fresh.compacted_summary.find("sidechat question") == std::string::npos);
    CHECK(fresh.items.empty());
    CHECK(fresh.title.starts_with("Sidechat · "));
}

TEST_CASE("refresh_sidechat requires an open sidechat")
{
    auto state = imza::test::make_test_state();
    refresh_sidechat(*state);
    CHECK(state->session->error().find("No Sidechat is open")
        != std::string::npos);
}

TEST_CASE("sidechat and main chat fill the height side by side")
{
    auto state = imza::test::make_test_state();
    open_sidechat(*state);
    auto sidechat_state = state->sidechat;
    REQUIRE(sidechat_state != nullptr);
    sidechat_state->session->begin_send("stretch me");

    SidechatStatus status;
    auto sidechat = make_sidechat_component(state, [] { }, status);

    using namespace ftxui;
    // Mirrors the Repl wide layout: title row above a side-by-side row
    // that must grow to the remaining height.
    Element row = hbox({ vbox({ text("MAIN") }) | xflex | yflex,
                      separatorEmpty(), sidechat->Render() })
        | yflex;
    Element root = vbox({ hbox({ text(" "), text("title") }), std::move(row),
                       text("STATUS") })
        | flex;
    auto screen                          = imza::test::to_screen(root, 120, 30);
    const std::vector<std::string> lines = imza::split_lines(screen.ToString());
    REQUIRE(static_cast<int>(lines.size()) == 30);
    // The sidechat input sits in the fixed right column; its hint line
    // ("Ctrl+S hide") must render near the bottom of the stretched pane,
    // not stop right below the short main content.
    int hint_row = -1;
    for (int y = 0; y < 30; ++y) {
        const std::string clean
            = imza::test::without_ansi(imza::split_lines(screen.ToString())[y]);
        if (clean.find("Ctrl+S hide") != std::string::npos) {
            hint_row = y;
        }
    }
    REQUIRE(hint_row != -1);
    CHECK(hint_row >= 25);
}

} // namespace imza
