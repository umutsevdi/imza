#include <doctest/doctest.h>

#include <string>

#include "platform/credential_subsystem.h"
#include "test_platform.h"

namespace {

// A key no other run or test uses, so a real keychain is left as found.
std::string unique_key()
{
    static int sequence = 0;
    return "imza-test-" + std::to_string(imza::test::process_id()) + "-"
        + std::to_string(++sequence);
}

} // namespace

TEST_CASE("plaintext mode settles immediately and stores nothing")
{
    imza::CredentialSubsystem store(imza::CredentialSubsystem::Mode::PLAINTEXT);
    CHECK(store.ready());
    CHECK_FALSE(store.is_keychain_available());
    CHECK(store.unavailable_reason().empty());

    imza::Status status = imza::Status::OK;
    CHECK(store.get("provider/test", status).empty());
    CHECK(status == imza::Status::NOT_FOUND);
    CHECK(store.set("provider/test", "value") == imza::Status::CONFIG_ERROR);
    CHECK(store.remove("provider/test") == imza::Status::CONFIG_ERROR);
    CHECK(store.has("provider/test") == imza::Status::NOT_FOUND);
    CHECK_FALSE(store.cached_secret("provider/test").has_value());
}

TEST_CASE("keychain round trip when one is available")
{
    imza::CredentialSubsystem store(imza::CredentialSubsystem::Mode::OS);
    if (!store.is_keychain_available()) {
        return;
    }
    const std::string key = unique_key();

    imza::Status status = imza::Status::OK;
    CHECK(store.get(key, status).empty());
    CHECK(status == imza::Status::NOT_FOUND);

    CHECK(store.set(key, "first") == imza::Status::OK);
    CHECK(store.has(key) == imza::Status::OK);
    CHECK(store.get(key, status) == "first");
    CHECK(status == imza::Status::OK);
    // set() caches, so a reader after the write never round-trips again.
    CHECK(store.cached_secret(key) == std::optional<std::string> { "first" });

    CHECK(store.set(key, "second") == imza::Status::OK);
    CHECK(store.get(key, status) == "second");

    CHECK(store.remove(key) == imza::Status::OK);
    CHECK(store.has(key) == imza::Status::NOT_FOUND);
    CHECK_FALSE(store.cached_secret(key).has_value());
    CHECK(store.remove(key) == imza::Status::NOT_FOUND);
}

TEST_CASE("keychain values are bounded")
{
    imza::CredentialSubsystem store(imza::CredentialSubsystem::Mode::OS);
    if (!store.is_keychain_available()) {
        return;
    }
    const std::string key = unique_key();
    CHECK(store.set(key, std::string(2561, 'x')) == imza::Status::CONFIG_ERROR);
    CHECK(store.has(key) == imza::Status::NOT_FOUND);
}

TEST_CASE("credential changes publish")
{
    imza::CredentialSubsystem store(imza::CredentialSubsystem::Mode::OS);
    if (!store.is_keychain_available()) {
        return;
    }
    // The worker's startup announcement must be behind us: subscribing
    // earlier would count it as a change.
    REQUIRE(store.wait_ready(std::chrono::seconds { 30 }));
    const std::string key = unique_key();
    int published         = 0;
    auto subscription     = store.subscribe_to_change([&] { ++published; });

    CHECK(store.set(key, "value") == imza::Status::OK);
    CHECK(published == 1);
    CHECK(store.remove(key) == imza::Status::OK);
    CHECK(published == 2);
}

TEST_CASE("the worker resolves a provider once and serves it from cache")
{
    imza::CredentialSubsystem probe(imza::CredentialSubsystem::Mode::OS);
    if (!probe.is_keychain_available()) {
        return;
    }
    const std::string id
        = "resolve-" + std::to_string(imza::test::process_id());
    const std::string key = "provider/" + id;
    REQUIRE(probe.set(key, "resolved") == imza::Status::OK);

    imza::Connection connection;
    connection.id = id;
    imza::CredentialSubsystem store(
        imza::CredentialSubsystem::Mode::OS, { connection }, { });
    REQUIRE(store.wait_ready(std::chrono::seconds { 30 }));
    CHECK(
        store.cached_secret(key) == std::optional<std::string> { "resolved" });

    imza::Status status = imza::Status::OK;
    CHECK(store.lookup(key, status) == "resolved");
    CHECK(status == imza::Status::OK);
    // A key the worker never resolved still falls through to the backend.
    CHECK(store.lookup("provider/absent", status).empty());
    CHECK(status == imza::Status::NOT_FOUND);

    CHECK(probe.remove(key) == imza::Status::OK);
}

TEST_CASE("stop unblocks a waiter that never saw the worker finish")
{
    auto store = std::make_unique<imza::CredentialSubsystem>(
        imza::CredentialSubsystem::Mode::OS);
    store->stop();
    CHECK(store->ready());
    CHECK(store->wait_ready(std::chrono::milliseconds { 10 }));
}
