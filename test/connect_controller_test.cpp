#include <doctest/doctest.h>

#include <unistd.h>

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <queue>
#include <thread>

#include "catalog.h"
#include "controller.h"
#include "types.h"

namespace {

struct IsolatedConfig {
    std::filesystem::path dir;
    std::string old_xdg;
    bool had_xdg = false;

    IsolatedConfig()
    {
        static int counter = 0;
        dir = std::filesystem::temp_directory_path()
            / ("ursa-ctrl-test-" + std::to_string(::getpid()) + "-"
                + std::to_string(counter++));
        std::filesystem::create_directories(dir);
        if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
            old_xdg = xdg;
            had_xdg = true;
        }
        setenv("XDG_CONFIG_HOME", dir.string().c_str(), 1);

        ursa::Catalog catalog;
        catalog.fetched_at
            = static_cast<std::int64_t>(std::time(nullptr));
        const auto src = ursa::parse_json(R"({
            "name": "Test Provider",
            "api": "http://127.0.0.1:9/v1",
            "npm": "@ai-sdk/openai-compatible"
        })");
        std::ignore = ursa::trim_provider(src,
            catalog.providers["testprov"]);
        std::ignore = ursa::save_catalog(
            dir / "ursa" / "presets.json", catalog);
    }

    ~IsolatedConfig()
    {
        if (had_xdg) {
            setenv("XDG_CONFIG_HOME", old_xdg.c_str(), 1);
        } else {
            unsetenv("XDG_CONFIG_HOME");
        }
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

struct PostPump {
    std::mutex mutex;
    std::queue<std::function<void()>> queue;

    ursa::PostFn fn()
    {
        return [this](std::function<void()> f) {
            std::lock_guard lock(mutex);
            queue.push(std::move(f));
        };
    }

    void drain()
    {
        for (;;) {
            std::function<void()> f;
            {
                std::lock_guard lock(mutex);
                if (queue.empty()) {
                    return;
                }
                f = std::move(queue.front());
                queue.pop();
            }
            f();
        }
    }

    template <typename Pred>
    bool wait_for(Pred pred)
    {
        for (int i = 0; i < 10000; ++i) {
            drain();
            if (pred()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }
};

ursa::ModelsFn fake_models_ok()
{
    return [](const ursa::Route&, std::vector<ursa::ModelInfo>& out) {
        out = { { "m1" }, { "m2" } };
        return ursa::Status::OK;
    };
}

ursa::ModelsFn fake_models_fail()
{
    return [](const ursa::Route&, std::vector<ursa::ModelInfo>&) {
        return ursa::Status::API_ERROR;
    };
}

} // namespace

TEST_CASE("connect commits a connection and lands models in the catalog")
{
    IsolatedConfig iso;
    PostPump pump;
    auto providers = std::make_shared<ursa::ProviderStore>(
        ursa::Config { }, fake_models_ok());
    ursa::Controller controller { std::make_shared<ursa::Session>(), providers,
        pump.fn(), [] { } };
    pump.drain();

    controller.resolve_modal(
        ursa::ModalResult { ursa::ConnectResult { "testprov", "", "key1",
            true } });
    REQUIRE(pump.wait_for([&] {
        const auto views = providers->connections();
        return views.size() == 1
            && views[0].state
            == ursa::ConnectionView::State::READY;
    }));

    const auto views = providers->connections();
    CHECK(views[0].id == "testprov");
    CHECK(views[0].provider_id == "testprov");
    CHECK(views[0].model_count == 2);
    CHECK(controller.session().connect_status() == "✓ 2 models");

    ursa::Config saved;
    REQUIRE(ursa::load_config(ursa::config_path(), saved)
        == ursa::Status::OK);
    REQUIRE(saved.providers.size() == 1);
    CHECK(saved.providers[0].provider_id == "testprov");
    CHECK(saved.providers[0].api_key == "key1");
}

TEST_CASE("connecting the same endpoint updates in place")
{
    IsolatedConfig iso;
    PostPump pump;
    auto providers = std::make_shared<ursa::ProviderStore>(
        ursa::Config { }, fake_models_ok());
    ursa::Controller controller { std::make_shared<ursa::Session>(), providers,
        pump.fn(), [] { } };
    pump.drain();

    controller.resolve_modal(
        ursa::ModalResult { ursa::ConnectResult { "testprov", "", "key1",
            true } });
    REQUIRE(pump.wait_for([&] {
        return providers->connections().size() == 1;
    }));

    controller.resolve_modal(
        ursa::ModalResult { ursa::ConnectResult { "testprov", "", "key2",
            true } });
    REQUIRE(pump.wait_for([&] {
        const auto views = providers->connections();
        return views.size() == 1 && views[0].api_key == "key2";
    }));
    pump.drain();

    const auto views = providers->connections();
    REQUIRE(views.size() == 1);
    CHECK(views[0].id == "testprov");
    CHECK(views[0].api_key == "key2");
}

TEST_CASE("test-only connect does not persist")
{
    IsolatedConfig iso;
    PostPump pump;
    auto providers = std::make_shared<ursa::ProviderStore>(
        ursa::Config { }, fake_models_ok());
    ursa::Controller controller { std::make_shared<ursa::Session>(), providers,
        pump.fn(), [] { } };
    pump.drain();

    controller.resolve_modal(
        ursa::ModalResult { ursa::ConnectResult { "testprov", "", "key",
            false } });
    REQUIRE(pump.wait_for([&] {
        return controller.session().connect_status() == "✓ 2 models";
    }));
    CHECK(providers->connections().empty());

    ursa::Config saved;
    REQUIRE(ursa::load_config(ursa::config_path(), saved)
        == ursa::Status::OK);
    CHECK(saved.providers.empty());
}

TEST_CASE("failing test keeps the connection absent")
{
    IsolatedConfig iso;
    PostPump pump;
    auto providers = std::make_shared<ursa::ProviderStore>(
        ursa::Config { }, fake_models_fail());
    ursa::Controller controller { std::make_shared<ursa::Session>(), providers,
        pump.fn(), [] { } };
    pump.drain();

    controller.resolve_modal(
        ursa::ModalResult { ursa::ConnectResult { "testprov", "", "key",
            true } });
    REQUIRE(pump.wait_for([&] {
        return !controller.session().connect_status().empty();
    }));
    CHECK(providers->connections().empty());
    CHECK(controller.session().connect_status() == "API error");
}

TEST_CASE("model pick sets last_used and persists")
{
    IsolatedConfig iso;
    PostPump pump;
    ursa::Config cfg;
    ursa::Connection conn;
    conn.id          = "testprov";
    conn.provider_id = "testprov";
    conn.api_key     = "k";
    cfg.providers.push_back(conn);

    auto providers
        = std::make_shared<ursa::ProviderStore>(cfg, fake_models_ok());
    ursa::Controller controller { std::make_shared<ursa::Session>(), providers,
        pump.fn(), [] { } };
    pump.drain();

    controller.resolve_modal(
        ursa::ModalResult { ursa::ModelChoice { "testprov", "m1" } });
    pump.drain();

    const auto snapshot = providers->config();
    REQUIRE(snapshot.last_used.has_value());
    CHECK(snapshot.last_used->provider == "testprov");
    CHECK(snapshot.last_used->model == "m1");

    ursa::Config saved;
    REQUIRE(ursa::load_config(ursa::config_path(), saved)
        == ursa::Status::OK);
    REQUIRE(saved.last_used.has_value());
    CHECK(saved.last_used->model == "m1");
}

TEST_CASE("provider store resolves configured subagent model")
{
    ursa::Config cfg;
    ursa::Connection connection;
    connection.id          = "configured";
    connection.provider_id = "openai";
    connection.endpoint    = "https://example.test/v1/chat/completions";
    connection.dialects["research-model"] = ursa::ApiStandard::ANTHROPIC;
    cfg.providers.push_back(connection);
    cfg.subagents[ursa::SubagentRole::RESEARCH]
        = { "configured", "research-model", "high" };
    ursa::ProviderStore providers(cfg, fake_models_ok());

    const auto selection
        = providers.subagent_selection(ursa::SubagentRole::RESEARCH);
    REQUIRE(selection.has_value());
    CHECK(selection->connection_id == "configured");
    CHECK(selection->model == "research-model");
    CHECK(selection->reasoning_effort == "high");
}

TEST_CASE("subagent defaults follow the active chat model")
{
    ursa::Config cfg;
    ursa::Connection connection;
    connection.id          = "configured";
    connection.provider_id = "openai";
    connection.endpoint    = "https://example.test/v1/chat/completions";
    cfg.providers.push_back(connection);
    cfg.last_used = ursa::LastUsed { "configured", "chat-model" };
    ursa::ProviderStore providers(cfg, fake_models_ok());

    const auto builder
        = providers.subagent_selection(ursa::SubagentRole::BUILDER);
    const auto research
        = providers.subagent_selection(ursa::SubagentRole::RESEARCH);
    const auto basic
        = providers.subagent_selection(ursa::SubagentRole::BASIC);
    REQUIRE(builder.has_value());
    REQUIRE(research.has_value());
    REQUIRE(basic.has_value());
    CHECK(builder->model == "chat-model");
    CHECK(builder->reasoning_effort == "medium");
    CHECK(research->reasoning_effort == "low");
    CHECK(basic->reasoning_effort == "off");
}

TEST_CASE("subagent configuration does not change main model reasoning")
{
    ursa::Config cfg;
    ursa::Connection connection;
    connection.id          = "configured";
    connection.provider_id = "openai";
    connection.endpoint    = "https://example.test/v1/chat/completions";
    cfg.providers.push_back(connection);
    cfg.last_used = ursa::LastUsed { "configured", "chat-model" };
    cfg.reasoning_effort = "high";
    cfg.subagents[ursa::SubagentRole::BASIC] = { "", "", "off" };
    ursa::ProviderStore providers(cfg, fake_models_ok());

    const auto main = providers.active_selection();
    REQUIRE(main.has_value());
    CHECK(main->model == "chat-model");
    CHECK(main->reasoning_effort == "high");
}

TEST_CASE("removing the active connection re-points last_used")
{
    IsolatedConfig iso;
    PostPump pump;
    ursa::Config cfg;
    ursa::Connection a;
    a.id          = "a";
    a.provider_id = "testprov";
    ursa::Connection b;
    b.id          = "b";
    b.provider_id = "testprov";
    cfg.providers = { a, b };
    cfg.last_used = ursa::LastUsed { "a", "m1" };

    auto providers
        = std::make_shared<ursa::ProviderStore>(cfg, fake_models_ok());
    ursa::Controller controller { std::make_shared<ursa::Session>(), providers,
        pump.fn(), [] { } };
    pump.drain();

    CHECK(providers->remove_connection("a"));
    pump.drain();

    const auto snapshot = providers->config();
    REQUIRE(snapshot.last_used.has_value());
    CHECK(snapshot.last_used->provider == "b");
    CHECK(snapshot.last_used->model.empty());
}

TEST_CASE("removing the last connection is refused")
{
    IsolatedConfig iso;
    PostPump pump;
    ursa::Config cfg;
    ursa::Connection a;
    a.id          = "a";
    a.provider_id = "testprov";
    cfg.providers = { a };

    auto providers
        = std::make_shared<ursa::ProviderStore>(cfg, fake_models_ok());
    ursa::Controller controller { std::make_shared<ursa::Session>(), providers,
        pump.fn(), [] { } };

    CHECK_FALSE(providers->remove_connection("a"));
    CHECK(providers->connections().size() == 1);
}

TEST_CASE("send guard blocks messages without an active model")
{
    IsolatedConfig iso;
    PostPump pump;
    auto providers = std::make_shared<ursa::ProviderStore>(
        ursa::Config { }, fake_models_ok());
    ursa::Controller controller { std::make_shared<ursa::Session>(), providers,
        pump.fn(), [] { } };
    pump.drain();

    controller.submit("hello");
    CHECK(controller.session().items().empty());
    CHECK(controller.session().error()
        == "no model selected — run /model");
}
