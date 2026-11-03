#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "platform/config.h"
#include "providers/catalog.h"
#include "providers/provider_store.h"
#include "providers/subscriptions.h"
#include "test_fs.h"
#include "test_platform.h"
#include "test_state.h"

namespace {

imza::Config subscription_config(
    std::int64_t expires_at, std::string refresh_token)
{
    imza::Connection connection;
    connection.id            = imza::OPENAI_SUBSCRIPTION_ID;
    connection.api_key       = "stale-token";
    connection.refresh_token = std::move(refresh_token);
    connection.expires_at    = expires_at;
    imza::Config config;
    config.providers.push_back(connection);
    return config;
}

std::int64_t now_plus(std::int64_t seconds)
{
    return static_cast<std::int64_t>(std::time(nullptr)) + seconds;
}

imza::SubscriptionResult refreshed_credentials()
{
    imza::SubscriptionResult result;
    result.status                    = imza::Status::OK;
    result.credentials.access_token  = "fresh-token";
    result.credentials.refresh_token = "fresh-refresh";
    result.credentials.expires_at    = now_plus(3600);
    result.credentials.account_id    = "acct";
    return result;
}

} // namespace

TEST_CASE("authenticated_route_for serves the cached token outside the "
          "refresh window")
{
    imza::test::IsolatedDataHome home;
    std::atomic<int> refresh_calls { 0 };
    imza::ProviderStore providers(
        imza::test::make_config_store(
            subscription_config(now_plus(3600), "r1")),
        { }, { }, [&](std::string_view, std::string_view, std::string_view) {
            ++refresh_calls;
            return refreshed_credentials();
        });

    const imza::Route route = providers.authenticated_route_for(
        imza::OPENAI_SUBSCRIPTION_ID, imza::ApiStandard::OPENAI_RESPONSES);

    CHECK(route.error == imza::Status::OK);
    CHECK(route.api_key == "stale-token");
    CHECK(refresh_calls.load() == 0);
}

TEST_CASE("authenticated_route_for fails closed without a refresh token")
{
    imza::test::IsolatedDataHome home;
    imza::ProviderStore providers(
        imza::test::make_config_store(subscription_config(now_plus(100), "")),
        { }, { }, [](std::string_view, std::string_view, std::string_view) {
            FAIL("refresh must not run without a refresh token");
            return imza::SubscriptionResult { };
        });

    const imza::Route route = providers.authenticated_route_for(
        imza::OPENAI_SUBSCRIPTION_ID, imza::ApiStandard::OPENAI_RESPONSES);

    CHECK(route.error == imza::Status::API_ERROR);
    CHECK(route.error_message == "Subscription expired. Sign in again.");
    CHECK(route.api_key == "stale-token");
}

TEST_CASE("authenticated_route_for refreshes an expiring token and "
          "persists it")
{
    imza::test::IsolatedDataHome home;
    std::atomic<int> refresh_calls { 0 };
    imza::ProviderStore providers(
        imza::test::make_config_store(subscription_config(now_plus(100), "r1")),
        { }, { }, [&](std::string_view, std::string_view, std::string_view) {
            ++refresh_calls;
            return refreshed_credentials();
        });

    const imza::Route route = providers.authenticated_route_for(
        imza::OPENAI_SUBSCRIPTION_ID, imza::ApiStandard::OPENAI_RESPONSES);

    CHECK(refresh_calls.load() == 1);
    CHECK(route.error == imza::Status::OK);
    CHECK(route.api_key == "fresh-token");

    const imza::Config config = providers.config();
    REQUIRE(config.providers.size() == 1);
    CHECK(config.providers[0].api_key == "fresh-token");
    CHECK(config.providers[0].refresh_token == "fresh-refresh");
    CHECK(config.providers[0].expires_at == now_plus(3600));
}

TEST_CASE("concurrent authenticated_route_for calls share one refresh")
{
    imza::test::IsolatedDataHome home;
    std::atomic<int> refresh_calls { 0 };
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool refresh_entered = false;
    bool gate_open       = false;

    imza::ProviderStore providers(
        imza::test::make_config_store(subscription_config(now_plus(100), "r1")),
        { }, { }, [&](std::string_view, std::string_view, std::string_view) {
            ++refresh_calls;
            std::unique_lock lock(gate_mutex);
            refresh_entered = true;
            gate_cv.notify_all();
            // Hold the refresh open until the second caller is parked on
            // the in-flight marker, then release the fresh credentials.
            gate_cv.wait(lock, [&] { return gate_open; });
            return refreshed_credentials();
        });

    std::thread first([&] {
        const imza::Route route = providers.authenticated_route_for(
            imza::OPENAI_SUBSCRIPTION_ID, imza::ApiStandard::OPENAI_RESPONSES);
        CHECK(route.api_key == "fresh-token");
    });
    {
        std::unique_lock lock(gate_mutex);
        REQUIRE(gate_cv.wait_for(
            lock, std::chrono::seconds(5), [&] { return refresh_entered; }));
    }
    std::thread second([&] {
        const imza::Route route = providers.authenticated_route_for(
            imza::OPENAI_SUBSCRIPTION_ID, imza::ApiStandard::OPENAI_RESPONSES);
        // Whether this caller refreshed or waited on the first one, it must
        // never observe the stale token once the refresh committed.
        CHECK(route.api_key == "fresh-token");
    });
    // The second caller is inside the 300s window: it either waits on the
    // in-flight marker or arrives after the refresh; give it a moment to
    // park before opening the gate.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    {
        std::lock_guard lock(gate_mutex);
        gate_open = true;
        gate_cv.notify_all();
    }
    first.join();
    second.join();

    CHECK(refresh_calls.load() == 1);
}

TEST_CASE("authenticated_route_for reports a failed refresh without "
          "losing the connection")
{
    imza::test::IsolatedDataHome home;
    imza::ProviderStore providers(
        imza::test::make_config_store(subscription_config(now_plus(100), "r1")),
        { }, { }, [](std::string_view, std::string_view, std::string_view) {
            imza::SubscriptionResult result;
            result.status = imza::Status::NETWORK_ERROR;
            result.error  = "offline";
            return result;
        });

    const imza::Route route = providers.authenticated_route_for(
        imza::OPENAI_SUBSCRIPTION_ID, imza::ApiStandard::OPENAI_RESPONSES);

    CHECK(route.error == imza::Status::NETWORK_ERROR);
    CHECK(route.error_message == "offline");
}

namespace {

using imza::test::CatalogDataHome;

// Counters so a test can prove which path the store took.
int models_calls = 0;
int verify_calls = 0;
std::string verified_model;
std::string verify_message;

imza::ModelsFn counting_models()
{
    return [](const imza::Route&, std::vector<imza::ModelInfo>&) {
        ++models_calls;
        return imza::Status::OK;
    };
}

imza::VerifyFn recording_verify(imza::Status result)
{
    return [result](const imza::Route&, std::string_view model,
               std::string* message) {
        ++verify_calls;
        verified_model = std::string(model);
        if (message != nullptr) {
            *message = verify_message;
        }
        return result;
    };
}

imza::ConnectOutcome connect_databricks(
    imza::ProviderStore& providers, std::string key = "key")
{
    imza::ConnectOutcome outcome;
    bool done = false;
    providers.connect(
        imza::ConnectResult { "databricks", "", std::move(key), "", true },
        [&](imza::ConnectOutcome result) {
            outcome = std::move(result);
            done    = true;
        });
    REQUIRE(imza::test::wait_until([&] { return done; }));
    return outcome;
}

} // namespace

TEST_CASE("a derived-endpoint provider lists from the catalog but still "
          "verifies the key")
{
    CatalogDataHome home("databricks");
    const imza::test::ScopedEnv host(
        "DATABRICKS_HOST", "dbc-123.cloud.databricks.com");
    models_calls = 0;
    verify_calls = 0;
    imza::ProviderStore providers(imza::test::make_config_store(),
        counting_models(), { }, { }, nullptr,
        recording_verify(imza::Status::OK));

    const imza::ConnectOutcome outcome = connect_databricks(providers);

    REQUIRE(outcome.status == imza::Status::OK);
    CHECK(outcome.persisted);
    // The model list is the catalog row; the credential is proven by a
    // separate one-token completion, since the endpoint serves no /models.
    CHECK(models_calls == 0);
    CHECK(verify_calls == 1);
    CHECK(verified_model == "catalog-model");
    CHECK(outcome.model_count == 1);
}

TEST_CASE("a rejected or unroutable derived-endpoint connect fails closed")
{
    CatalogDataHome home("databricks");
    const imza::test::ScopedEnv host(
        "DATABRICKS_HOST", "dbc-123.cloud.databricks.com");
    models_calls   = 0;
    verify_calls   = 0;
    verify_message = "invalid api key";
    imza::ProviderStore providers(imza::test::make_config_store(),
        counting_models(), { }, { }, nullptr,
        recording_verify(imza::Status::API_ERROR));

    // A random string must not be accepted just because the catalog has a
    // model list for the provider, and the provider's own words must reach
    // the user rather than a bare "API error".
    const imza::ConnectOutcome rejected
        = connect_databricks(providers, "not-a-real-key");
    CHECK(rejected.status == imza::Status::API_ERROR);
    CHECK_FALSE(rejected.persisted);
    CHECK(rejected.message == "invalid api key");
    CHECK(verify_calls == 1);
    CHECK(providers.connections().empty());

    // Without its variable the connection cannot route at all, so it never
    // reaches verification and must not report a model list.
    imza::test::unset_env("DATABRICKS_HOST");
    imza::ProviderStore unroutable(
        imza::test::make_config_store(), counting_models());
    const imza::ConnectOutcome refused = connect_databricks(unroutable);
    CHECK(refused.status == imza::Status::CONFIG_ERROR);
    CHECK(refused.message.find("DATABRICKS_HOST") != std::string::npos);
    CHECK(unroutable.connections().empty());

    imza::Config config;
    imza::Connection connection;
    connection.id = "databricks";
    config.providers.push_back(connection);
    imza::ProviderStore stored(
        imza::test::make_config_store(config), counting_models());
    stored.start_model_fetches();
    const auto models = stored.models_for("databricks");
    CHECK(models.state == imza::ModelList::State::FAILED);
    CHECK(models.error == imza::Status::CONFIG_ERROR);
}
