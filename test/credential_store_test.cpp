#include <doctest/doctest.h>

#include <unistd.h>

#include <string>

#include "platform/credential_store.h"

namespace {

// A key no other run or test uses, so a real keychain is left as found.
std::string unique_key()
{
    static int sequence = 0;
    return "imza-test-" + std::to_string(::getpid()) + "-"
        + std::to_string(++sequence);
}

} // namespace

TEST_CASE("plaintext mode never probes and stores nothing")
{
    imza::CredentialStore store(imza::CredentialStore::Mode::PLAINTEXT);
    CHECK_FALSE(store.is_keychain_available());
    // An explicit choice is not a degradation, so nothing is reported.
    CHECK(store.unavailable_reason().empty());

    imza::Status status = imza::Status::OK;
    CHECK(store.get("provider/test", status).empty());
    CHECK(status == imza::Status::NOT_FOUND);
    CHECK(store.set("provider/test", "value") == imza::Status::CONFIG_ERROR);
    CHECK(store.remove("provider/test") == imza::Status::CONFIG_ERROR);
    CHECK(store.has("provider/test") == imza::Status::NOT_FOUND);
}

TEST_CASE("a failed probe reports a reason, a successful one does not")
{
    imza::CredentialStore store(imza::CredentialStore::Mode::OS);
    CHECK(store.is_keychain_available() == store.unavailable_reason().empty());
}

TEST_CASE("keychain round trip when one is available")
{
    imza::CredentialStore store(imza::CredentialStore::Mode::OS);
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

    CHECK(store.set(key, "second") == imza::Status::OK);
    CHECK(store.get(key, status) == "second");

    CHECK(store.remove(key) == imza::Status::OK);
    CHECK(store.has(key) == imza::Status::NOT_FOUND);
    CHECK(store.remove(key) == imza::Status::NOT_FOUND);
}

TEST_CASE("keychain values are bounded")
{
    imza::CredentialStore store(imza::CredentialStore::Mode::OS);
    if (!store.is_keychain_available()) {
        return;
    }
    const std::string key = unique_key();
    CHECK(store.set(key, std::string(2561, 'x')) == imza::Status::CONFIG_ERROR);
    CHECK(store.has(key) == imza::Status::NOT_FOUND);
}

TEST_CASE("credential changes publish")
{
    imza::CredentialStore store(imza::CredentialStore::Mode::OS);
    if (!store.is_keychain_available()) {
        return;
    }
    const std::string key = unique_key();
    int published         = 0;
    auto subscription     = store.subscribe_to_change([&] { ++published; });

    CHECK(store.set(key, "value") == imza::Status::OK);
    CHECK(published == 1);
    CHECK(store.remove(key) == imza::Status::OK);
    CHECK(published == 2);
}
