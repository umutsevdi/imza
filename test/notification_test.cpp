#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <doctest/doctest.h>

#include "app/application_state.h"
#include "app/flows.h"
#include "platform/command_runner.h"
#include "runtime/notification_store.h"
#include "test_fs.h"
#include "test_helpers.h"
#include "test_platform.h"
#include "test_state.h"
#include "ui/ui.h"
#include "workspace/environment.h"
#include "workspace/git.h"

using imza::test::to_text;
using imza::test::without_ansi;

namespace {

using imza::Notification;
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
    const std::size_t first  = store.publish(false, "one");
    const std::size_t second = store.publish(true, "two");
    CHECK(first != 0);
    CHECK(second != first);

    CHECK(store.publish(false, "") == 0);

    store.dismiss(9999);
    CHECK(entries_of(store).size() == 2);
    store.dismiss(first);
    const auto entries = entries_of(store);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].description == "two");

    for (std::size_t i = 0; i < imza::MAX_NOTIFICATIONS + 3; ++i) {
        store.publish(false, "n" + std::to_string(i));
    }
    const auto bounded = entries_of(store);
    REQUIRE(bounded.size() == imza::MAX_NOTIFICATIONS);
    CHECK(bounded.back().description
        == "n" + std::to_string(imza::MAX_NOTIFICATIONS + 2));
}

TEST_CASE("the error flag selects the lifetime and every entry expires")
{
    NotificationStore store;
    store.publish(false, "info");
    store.publish(true, "error");

    const auto entries = entries_of(store);
    REQUIRE(entries.size() == 2);
    // Nothing persists: both carry a deadline and the window it came from.
    CHECK(entries[0].ttl == imza::INFO_TTL);
    CHECK(entries[1].ttl == imza::ERROR_TTL);
    CHECK(entries[0].expires_at > std::chrono::steady_clock::now());
    CHECK(entries[1].expires_at > entries[0].expires_at);
}

TEST_CASE("expiry drops only the elapsed entries")
{
    NotificationStore store;
    store.publish(true, "standing");

    CHECK_FALSE(store.dismiss_expired());
    // A zero-length window is already elapsed on the next sweep, so the
    // test never waits on the clock.
    store.publish(false, "expired", std::chrono::milliseconds { 0 });
    CHECK(store.dismiss_expired());

    const auto entries = entries_of(store);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].description == "standing");
    CHECK_FALSE(store.dismiss_expired());
}

TEST_CASE("a held snapshot is not mutated by later publishes")
{
    NotificationStore store;
    store.publish(false, "first");
    const auto held = store.notifications();

    store.publish(false, "second");
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

    const std::size_t id = store.publish(false, "one");
    CHECK(changes == 1);
    store.dismiss(id);
    CHECK(changes == 2);
}

// A repository whose upstream already holds every commit: the shape a
// second Push button press sees.
void seed_pushed_repo(
    const std::filesystem::path& root, const std::filesystem::path& origin)
{
    imza::test::init_test_repo(root, true);
    const auto git = [&root](const std::string& arguments) {
        REQUIRE(imza::command_ok(imza::run_command(
            imza::git_command(root, arguments), std::chrono::seconds { 10 },
            { }, { }, imza::StderrCapture::STDOUT_ONLY)));
    };
    REQUIRE(imza::command_ok(
        imza::run_command(imza::git_command(origin, "init -q --bare"),
            std::chrono::seconds { 10 }, { }, { },
            imza::StderrCapture::STDOUT_ONLY)));
    git("remote add origin " + imza::shell_quote(origin.string()));
    git("push -q -u origin HEAD");
}

TEST_CASE("pushing an up-to-date branch never claims a push")
{
    if (!imza::test::git_available()) {
        return;
    }
    imza::test::TempDir root;
    imza::test::TempDir origin;
    seed_pushed_repo(root.path, origin.path);

    const imza::test::CurrentDirectory keep;
    imza::test::PostPump pump;
    auto state = imza::test::make_test_state(pump.fn());
    std::filesystem::current_path(root.path);
    REQUIRE(state->environment->chdir(root.path)
        == imza::Environment::ChdirResult::CHANGED);
    imza::test::wait_for_environment(*state);

    imza::push_and_notify(*state);
    REQUIRE(pump.wait_for(
        [&] { return !state->notifications->notifications()->empty(); }));

    const auto entries = entries_of(*state->notifications);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].description == "Already up to date.");
    CHECK_FALSE(entries[0].error);
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
    CHECK(entries[0].error);
    CHECK(entries[0].description == "Something broke.");

    // The notice is dismissed by the user, not by session bookkeeping.
    state->session->clear_error();
    CHECK(entries_of(*state->notifications).size() == 1);
}
