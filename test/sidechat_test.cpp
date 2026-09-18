#include "app/application_state.h"
#include "app/flows.h"
#include "common/modal.h"
#include "platform/config.h"

#include <doctest/doctest.h>

namespace imza {

namespace {

    std::shared_ptr<ApplicationState> make_root()
    {
        return make_application_state(
            [](std::function<void()> f) { f(); }, Config { });
    }

} // namespace

TEST_CASE("sidechat toggle hides and reopens without discarding the session")
{
    auto state = make_root();
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
    auto state = make_root();
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
    auto state = make_root();
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
    auto state = make_root();
    refresh_sidechat(*state);
    CHECK(state->session->error().find("No Sidechat is open")
        != std::string::npos);
}

} // namespace imza