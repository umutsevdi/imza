#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <doctest/doctest.h>

#include "app/application_state.h"
#include "app/flows.h"
#include "runtime/notification_center.h"
#include "test_helpers.h"
#include "test_state.h"
#include "ui/ui.h"

using imza::test::to_text;
using imza::test::without_ansi;

namespace {

using imza::Notification;
using imza::NotificationLevel;
using imza::NotificationStore;

std::vector<Notification> entries_of(const NotificationStore& store)
{
    const auto snapshot = store.notifications();
    return std::vector<Notification>(snapshot->begin(), snapshot->end());
}

} // namespace

TEST_CASE("the store publishes, dismisses by id, and bounds itself")
{
    NotificationStore store;
    const std::size_t first  = store.publish(NotificationLevel::INFO, "one");
    const std::size_t second = store.publish(NotificationLevel::ERROR, "two");
    CHECK(first != 0);
    CHECK(second != first);

    // An empty description is not an entry.
    CHECK(store.publish(NotificationLevel::INFO, "") == 0);

    // An unknown id is a no-op; a known one removes exactly that entry.
    store.dismiss(9999);
    CHECK(entries_of(store).size() == 2);
    store.dismiss(first);
    const auto entries = entries_of(store);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].description == "two");

    // Past the cap the oldest entries are dropped, newest kept.
    for (std::size_t i = 0; i < imza::MAX_NOTIFICATIONS + 3; ++i) {
        store.publish(NotificationLevel::INFO, "n" + std::to_string(i));
    }
    const auto bounded = entries_of(store);
    REQUIRE(bounded.size() == imza::MAX_NOTIFICATIONS);
    CHECK(bounded.back().description
        == "n" + std::to_string(imza::MAX_NOTIFICATIONS + 2));
}

TEST_CASE("the level sets the lifetime and an explicit ttl overrides it")
{
    NotificationStore store;
    store.publish(NotificationLevel::INFO, "info");
    store.publish(NotificationLevel::WARN, "warn");
    store.publish(NotificationLevel::ERROR, "error");
    // NOTIFICATION_PERSIST keeps a transient level alive; a short ttl pins
    // a persistent one.
    store.publish(NotificationLevel::INFO, "standing",
        imza::NOTIFICATION_PERSIST);
    store.publish(
        NotificationLevel::ERROR, "pinned", std::chrono::milliseconds { 1 });

    const auto entries = entries_of(store);
    REQUIRE(entries.size() == 5);
    CHECK(entries[0].expires_at.has_value());
    CHECK(entries[1].expires_at.has_value());
    CHECK_FALSE(entries[2].expires_at.has_value());
    CHECK_FALSE(entries[3].expires_at.has_value());
    CHECK(entries[4].expires_at.has_value());
}

TEST_CASE("expiry drops only the elapsed entries")
{
    NotificationStore store;
    store.publish(
        NotificationLevel::INFO, "transient", std::chrono::milliseconds { 10 });
    store.publish(NotificationLevel::ERROR, "persistent");
    // A pending deadline is what keeps the UI frame loop alive.
    CHECK(store.has_pending_expiry());

    CHECK_FALSE(store.dismiss_expired());
    std::this_thread::sleep_for(std::chrono::milliseconds { 30 });
    CHECK(store.dismiss_expired());

    const auto entries = entries_of(store);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].description == "persistent");
    CHECK_FALSE(store.has_pending_expiry());
    CHECK_FALSE(store.dismiss_expired());
}

TEST_CASE("a held snapshot is not mutated by later publishes")
{
    NotificationStore store;
    store.publish(NotificationLevel::INFO, "first");
    const auto held = store.notifications();

    store.publish(NotificationLevel::INFO, "second");
    store.dismiss(held->front().id);

    REQUIRE(held->size() == 1);
    CHECK(held->front().description == "first");
    const auto current = entries_of(store);
    REQUIRE(current.size() == 1);
    CHECK(current.front().description == "second");
}

TEST_CASE("publishing and dismissing announce through the change signal")
{
    NotificationStore store;
    int changes       = 0;
    auto subscription = store.subscribe_to_change([&changes] { ++changes; });

    const std::size_t id = store.publish(NotificationLevel::INFO, "one");
    CHECK(changes == 1);
    store.dismiss(id);
    CHECK(changes == 2);
}

TEST_CASE("an alert rings the terminal without adding an overlay entry")
{
    NotificationStore store;
    std::vector<std::string> alerts;
    auto subscription = store.subscribe_to_alert(
        [&alerts](const std::string& message) { alerts.push_back(message); });

    store.alert("Imza finished");
    REQUIRE(alerts.size() == 1);
    CHECK(alerts.front() == "Imza finished");
    CHECK(store.notifications()->empty());

    // An empty message is dropped rather than ringing the bell for nothing.
    store.alert("");
    CHECK(alerts.size() == 1);
}

TEST_CASE("only the root attended session alerts on turn completion")
{
    auto root = imza::test::make_test_state();
    std::vector<std::string> alerts;
    auto subscription = root->notifications->subscribe_to_alert(
        [&alerts](const std::string& message) { alerts.push_back(message); });

    // The sidechat shares the store but carries an agent label, so its turn
    // completion must not ring the terminal.
    auto sidechat = imza::make_sidechat_application_state(*root);
    CHECK(sidechat->notifications == root->notifications);
    sidechat->session->begin_send("child");
    imza::on_turn_finished(*sidechat, "");
    CHECK(alerts.empty());

    root->session->begin_send("root");
    imza::on_turn_finished(*root, "");
    REQUIRE(alerts.size() == 1);
    CHECK(alerts.front() == "Imza finished");
}

TEST_CASE("a session error publishes and outlives clear_error")
{
    auto state = imza::test::make_test_state();
    state->session->set_error("Something broke.");

    const auto entries = entries_of(*state->notifications);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].level == NotificationLevel::ERROR);
    CHECK(entries[0].description == "Something broke.");

    // The notice is dismissed by the user, not by session bookkeeping.
    state->session->clear_error();
    CHECK(entries_of(*state->notifications).size() == 1);
}

TEST_CASE("the overlay renders one dismissible row per notice")
{
    imza::Session session;
    session.mark_retry(7, false);
    const std::vector<Notification> entries {
        Notification {
            1, NotificationLevel::INFO, "copied to clipboard", std::nullopt },
        Notification {
            2, NotificationLevel::ERROR, "something broke", std::nullopt },
    };

    std::map<std::size_t, ftxui::Box> boxes;
    const std::string out = without_ansi(
        to_text(imza::render_notifications(entries, session, boxes), 60, 10));

    // Descriptions render sentence-cased, like the old per-pane error line.
    CHECK(out.find("Copied to clipboard.") != std::string::npos);
    CHECK(out.find("Something broke.") != std::string::npos);
    // The retry countdown is live session state, not a store entry, so it
    // renders as its own row keyed to id 0.
    CHECK(out.find("Rate limited") != std::string::npos);
    CHECK(boxes.count(0) == 1);
    CHECK(boxes.count(1) == 1);
    CHECK(boxes.count(2) == 1);
}
