#include <doctest/doctest.h>

#include <unistd.h>

#include <filesystem>
#include <string>

#include "platform/config.h"
#include "platform/config_store.h"
#include "platform/credential_store.h"
#include "test_fs.h"

namespace {

imza::Config config_with_server(const std::string& id)
{
    imza::McpServerConfig server;
    server.id  = id;
    server.url = "https://example.test/mcp";
    imza::Config config;
    config.mcp_servers[id] = server;
    return config;
}

} // namespace

TEST_CASE("ConfigStore publishes a new snapshot on commit")
{
    const imza::test::TempDir dir;
    const std::filesystem::path path = dir.file("config.json");
    imza::ConfigStore store(imza::Config { }, path);

    int notifications = 0;
    auto subscription
        = store.subscribe_to_change([&notifications] { ++notifications; });

    CHECK(store.snapshot()->mcp_servers.empty());
    CHECK(store.update([&](imza::Config& config) {
        config.mcp_servers["alpha"]
            = config_with_server("alpha").mcp_servers.at("alpha");
        return true;
    }) == imza::ConfigUpdateResult::UPDATED);

    CHECK(notifications == 1);
    CHECK(store.snapshot()->mcp_servers.contains("alpha"));
}

TEST_CASE("ConfigStore snapshot reflects a write made through another handle")
{
    // The regression this guards: a reader holding the store's own copy
    // must observe a mutation that went through the same store, so no
    // component keeps a stale Config after an MCP add or remove.
    const imza::test::TempDir dir;
    const std::filesystem::path path = dir.file("config.json");
    auto store                       = std::make_shared<imza::ConfigStore>(
        config_with_server("alpha"), path);

    const std::shared_ptr<const imza::Config> before = store->snapshot();
    CHECK(before->mcp_servers.contains("alpha"));

    // A concurrent writer, as a second process would be.
    imza::ConfigStore other(config_with_server("alpha"), path);
    CHECK(other.update([&](imza::Config& config) {
        config.mcp_servers.erase("alpha");
        config.mcp_servers["beta"]
            = config_with_server("beta").mcp_servers.at("beta");
        return true;
    }) == imza::ConfigUpdateResult::UPDATED);

    // A read through the first handle sees the newest on-disk state.
    CHECK(store->update([](imza::Config&) { return false; })
        == imza::ConfigUpdateResult::UNCHANGED);
    CHECK_FALSE(store->snapshot()->mcp_servers.contains("alpha"));
    CHECK(store->snapshot()->mcp_servers.contains("beta"));

    // The snapshot taken before the write is immutable, not mutated in
    // place; a reader that still holds it keeps the old view.
    CHECK(before->mcp_servers.contains("alpha"));
}

TEST_CASE("ConfigStore syncs changed and removed connections to the keychain")
{
    imza::CredentialStore store(imza::CredentialStore::Mode::OS);
    if (!store.is_keychain_available()) {
        return;
    }
    const imza::test::TempDir dir;
    const std::filesystem::path path = dir.file("config.json");
    imza::ConfigStore config(imza::Config { }, path, &store);
    const std::string id  = "sync-" + std::to_string(::getpid());
    const std::string key = "provider/" + id;

    CHECK(config.update([&id](imza::Config& cfg) {
        imza::Connection connection;
        connection.id      = id;
        connection.api_key = "first";
        cfg.providers.push_back(connection);
        return true;
    }) == imza::ConfigUpdateResult::UPDATED);
    imza::Status status = imza::Status::OK;
    CHECK(store.get(key, status) == "first");
    CHECK(status == imza::Status::OK);

    CHECK(config.update([](imza::Config& cfg) {
        cfg.providers[0].api_key = "second";
        return true;
    }) == imza::ConfigUpdateResult::UPDATED);
    CHECK(store.get(key, status) == "second");

    CHECK(config.update([](imza::Config& cfg) {
        cfg.providers[0].label = "renamed";
        return true;
    }) == imza::ConfigUpdateResult::UPDATED);

    CHECK(config.update([](imza::Config& cfg) {
        cfg.providers.clear();
        return true;
    }) == imza::ConfigUpdateResult::UPDATED);
    CHECK(store.has(key) == imza::Status::NOT_FOUND);
}

TEST_CASE("ConfigStore leaves the snapshot untouched on a failed update")
{
    const imza::test::TempDir dir;
    // A directory where the file should be: every write fails.
    const std::filesystem::path path = dir.file("config.json");
    std::filesystem::create_directories(path);
    imza::ConfigStore store(config_with_server("alpha"), path);

    const std::shared_ptr<const imza::Config> before = store.snapshot();
    CHECK(store.update([](imza::Config&) { return true; })
        == imza::ConfigUpdateResult::FAILURE);
    CHECK(store.snapshot() == before);
    CHECK(store.snapshot()->mcp_servers.contains("alpha"));
}
