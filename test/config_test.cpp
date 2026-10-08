#include <doctest/doctest.h>

#include <unistd.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include "network/json.h"
#include "network/json_io.h"
#include "permissions/store.h"
#include "platform/config.h"
#include "test_fs.h"

namespace {

using imza::test::TempDir;

std::string json_str(const imza::JsonValue* v)
{
    return v != nullptr && v->is_string() ? v->as<std::string>() : "";
}

bool json_has(const imza::JsonValue& v, std::string_view key)
{
    return imza::find_member(v, key) != nullptr;
}

std::filesystem::path temp_file(const std::string& name)
{
    static TempDir dir;
    return dir.file(name);
}

} // namespace

static_assert(
    imza::subagent_default_variant(imza::SubagentRole::BUILDER) == "medium");
static_assert(
    imza::subagent_default_variant(imza::SubagentRole::RESEARCH) == "low");
static_assert(
    imza::subagent_default_variant(imza::SubagentRole::BASIC) == "off");

TEST_CASE("load_config missing file yields empty config")
{
    const auto path = temp_file("missing.json");
    imza::Config cfg;
    std::string error;
    CHECK(imza::load_config(path, cfg, &error) == imza::Status::OK);
    CHECK(cfg.providers.empty());
    CHECK_FALSE(cfg.last_used.has_value());
}

TEST_CASE("load_config corrupt JSON fails")
{
    const auto path = temp_file("corrupt.json");
    {
        std::ofstream out(path);
        out << "{ not json";
    }
    imza::Config cfg;
    std::string error;
    CHECK(imza::load_config(path, cfg, &error) == imza::Status::CONFIG_ERROR);
    CHECK_FALSE(error.empty());
}

TEST_CASE("load_config legacy-less body without providers is empty")
{
    const auto path = temp_file("empty.json");
    {
        std::ofstream out(path);
        out << "{\"other\": 1}";
    }
    imza::Config cfg;
    CHECK(imza::load_config(path, cfg) == imza::Status::OK);
    CHECK(cfg.providers.empty());
}

TEST_CASE("config roundtrip preserves connections and last_used")
{
    const auto path = temp_file("roundtrip.json");
    imza::Config cfg;
    imza::Connection conn;
    conn.id            = "openrouter";
    conn.api_key       = "sk-or-test";
    conn.refresh_token = "refresh-test";
    conn.expires_at    = 1756390000;
    conn.account_id    = "account-test";
    cfg.providers.push_back(conn);

    imza::Connection local;
    local.id                  = "local";
    local.endpoint            = "http://localhost:11434/v1/chat/completions";
    local.label               = "my Ollama";
    local.dialects["glm-5.3"] = imza::ApiStandard::ANTHROPIC;
    local.dialects["gpt-responses"] = imza::ApiStandard::OPENAI_RESPONSES;
    cfg.providers.push_back(local);

    cfg.last_used        = imza::LastUsed { "openrouter", "" };
    cfg.reasoning_effort = "high";

    REQUIRE(imza::save_config(path, cfg) == imza::Status::OK);

    imza::Config loaded;
    CHECK(imza::load_config(path, loaded) == imza::Status::OK);
    REQUIRE(loaded.providers.size() == 2);
    CHECK(loaded.providers[0].id == "openrouter");
    CHECK(loaded.providers[0].api_key == "sk-or-test");
    CHECK(loaded.providers[0].refresh_token == "refresh-test");
    CHECK(loaded.providers[0].expires_at == 1756390000);
    CHECK(loaded.providers[0].account_id == "account-test");
    CHECK(loaded.providers[0].endpoint.empty());
    CHECK(loaded.providers[0].label.empty());
    CHECK(loaded.providers[1].label == "my Ollama");
    CHECK(loaded.providers[1].endpoint
        == "http://localhost:11434/v1/chat/completions");
    REQUIRE(loaded.providers[1].dialects.count("glm-5.3") == 1);
    CHECK(loaded.providers[1].dialects.at("glm-5.3")
        == imza::ApiStandard::ANTHROPIC);
    CHECK(loaded.providers[1].dialects.at("gpt-responses")
        == imza::ApiStandard::OPENAI_RESPONSES);
    REQUIRE(loaded.last_used.has_value());
    CHECK(loaded.last_used->provider == "openrouter");
    CHECK(loaded.last_used->model.empty());
    CHECK(loaded.reasoning_effort == "high");
    const imza::JsonValue written
        = imza::parse_json(imza::test::read_all(path));
    REQUIRE(written.is_object());
    const imza::JsonValue* providers = imza::find_member(written, "providers");
    REQUIRE(providers != nullptr);
    REQUIRE(providers->is_array());
    const auto& provider_list = providers->get<imza::JsonValue::array_t>();
    REQUIRE(provider_list.size() == 2);
    CHECK(json_str(imza::find_member(provider_list[0], "id")) == "openrouter");
    CHECK_FALSE(json_has(provider_list[0], "active"));
    CHECK_FALSE(json_has(provider_list[0], "provider_id"));
    CHECK_FALSE(json_has(provider_list[0], "endpoint"));
    CHECK_FALSE(json_has(provider_list[0], "label"));
    CHECK_FALSE(json_has(provider_list[0], "dialects"));
    CHECK_FALSE(json_has(provider_list[1], "api_key"));
    CHECK_FALSE(json_has(provider_list[1], "refresh_token"));
    CHECK_FALSE(json_has(provider_list[1], "expires_at"));
    CHECK_FALSE(json_has(provider_list[1], "account_id"));
    CHECK(
        json_str(imza::find_member(provider_list[1], "label")) == "my Ollama");
    const imza::JsonValue* dialects
        = imza::find_member(provider_list[1], "dialects");
    REQUIRE(dialects != nullptr);
    CHECK(json_str(imza::find_member(*dialects, "gpt-responses"))
        == "openai-responses");
    const imza::JsonValue* models = imza::find_member(written, "models");
    REQUIRE(models != nullptr);
    const imza::JsonValue* main = imza::find_member(*models, "main");
    REQUIRE(main != nullptr);
    CHECK(json_str(imza::find_member(*main, "provider")) == "openrouter");
    CHECK(json_str(imza::find_member(*main, "reasoning_effort")) == "high");
    CHECK_FALSE(json_has(written, "last_used"));
    CHECK_FALSE(json_has(written, "reasoning_effort"));
}

TEST_CASE("load_config rejects providers without id")
{
    const auto path = temp_file("invalid.json");
    {
        std::ofstream out(path);
        out << R"({"providers": [{"id": ""}]})";
    }
    imza::Config cfg;
    CHECK(imza::load_config(path, cfg) == imza::Status::CONFIG_ERROR);
}

TEST_CASE("load_config accepts legacy provider_id as id")
{
    const auto path = temp_file("legacy-provider-id.json");
    {
        std::ofstream out(path);
        out << R"({"providers": [{"provider_id": "openai", "api_key": "k"}]})";
    }
    imza::Config cfg;
    REQUIRE(imza::load_config(path, cfg) == imza::Status::OK);
    REQUIRE(cfg.providers.size() == 1);
    CHECK(cfg.providers[0].id == "openai");
    CHECK(cfg.providers[0].api_key == "k");
}

TEST_CASE("load_config disambiguates duplicate connections with labels")
{
    const auto path = temp_file("duplicate-provider-id.json");
    {
        std::ofstream out(path);
        out << R"({"providers": [)"
               R"({"id": "openai"}, {"id": "openai"}, )"
               R"({"id": "custom", "label": "one"}, )"
               R"({"id": "custom", "label": "one"}, )"
               R"({"id": "custom", "label": "one"}]})";
    }
    imza::Config cfg;
    REQUIRE(imza::load_config(path, cfg) == imza::Status::OK);
    REQUIRE(cfg.providers.size() == 5);
    CHECK(cfg.providers[0].label.empty());
    CHECK(cfg.providers[1].label == "2");
    CHECK(cfg.providers[2].label == "one");
    CHECK(cfg.providers[3].label == "one 2");
    CHECK(cfg.providers[4].label == "one 3");
    CHECK(imza::connection_key(cfg.providers[0]) == "openai");
    CHECK(imza::connection_key(cfg.providers[1]) == "openai/2");
    CHECK(imza::connection_key(cfg.providers[2]) == "custom/one");
    CHECK(imza::connection_key(cfg.providers[3]) == "custom/one 2");
    CHECK(imza::connection_key(cfg.providers[4]) == "custom/one 3");
}

TEST_CASE("config roundtrip resolves labeled connection keys")
{
    const auto path = temp_file("labeled-connections.json");
    imza::Config cfg;
    imza::Connection personal;
    personal.id    = "openai-subscription";
    personal.label = "plus";
    cfg.providers.push_back(std::move(personal));
    imza::Connection work;
    work.id    = "openai-subscription";
    work.label = "work";
    cfg.providers.push_back(std::move(work));
    cfg.last_used = imza::LastUsed { "openai-subscription/plus", "gpt-main" };
    cfg.subagents[imza::SubagentRole::RESEARCH]
        = { "openai-subscription/work", "gpt-research", "low" };

    REQUIRE(imza::save_config(path, cfg) == imza::Status::OK);

    imza::Config loaded;
    REQUIRE(imza::load_config(path, loaded) == imza::Status::OK);
    REQUIRE(loaded.last_used.has_value());
    CHECK(loaded.last_used->provider == "openai-subscription/plus");
    REQUIRE(loaded.subagents.contains(imza::SubagentRole::RESEARCH));
    CHECK(loaded.subagents.at(imza::SubagentRole::RESEARCH).provider
        == "openai-subscription/work");
}

TEST_CASE("load_config rejects unknown dialect")
{
    const auto path = temp_file("dialect.json");
    {
        std::ofstream out(path);
        out << R"({"providers": [{"id": "a", "dialects": {"m": "grpc"}}]})";
    }
    imza::Config cfg;
    CHECK(imza::load_config(path, cfg) == imza::Status::CONFIG_ERROR);
}

TEST_CASE("load_config rejects unresolved models main provider")
{
    const auto path = temp_file("last.json");
    {
        std::ofstream out(path);
        out << R"({"providers": [], "models": {"main": {"provider": "x",
            "model": "m"}}})";
    }
    imza::Config cfg;
    CHECK(imza::load_config(path, cfg) == imza::Status::CONFIG_ERROR);
}

TEST_CASE("save_config creates parent directories")
{
    auto path = std::filesystem::temp_directory_path()
        / ("imza-config-nested-" + std::to_string(::getpid())) / "deep"
        / "nested" / "config.json";
    std::filesystem::remove_all(path.parent_path().parent_path());

    imza::Config cfg;
    CHECK(imza::save_config(path, cfg) == imza::Status::OK);
    CHECK(std::filesystem::exists(path));

    imza::Config loaded;
    CHECK(imza::load_config(path, loaded) == imza::Status::OK);
    CHECK(loaded.providers.empty());
    std::filesystem::remove_all(path.parent_path().parent_path());
}

TEST_CASE("empty config writes every editable option")
{
    const auto path = temp_file("complete-defaults.json");
    REQUIRE(imza::save_config(path, imza::Config { }) == imza::Status::OK);

    const imza::JsonValue written
        = imza::parse_json(imza::test::read_all(path));
    REQUIRE(written.is_object());
    const imza::JsonValue* providers = imza::find_member(written, "providers");
    REQUIRE(providers != nullptr);
    REQUIRE(providers->is_array());
    const imza::JsonValue* models = imza::find_member(written, "models");
    REQUIRE(models != nullptr);
    const auto check_effort
        = [models](std::string_view key, std::string_view expected) {
              const imza::JsonValue* section = imza::find_member(*models, key);
              REQUIRE(section != nullptr);
              CHECK(json_str(imza::find_member(*section, "reasoning_effort"))
                  == expected);
          };
    const imza::JsonValue* main = imza::find_member(*models, "main");
    REQUIRE(main != nullptr);
    CHECK_FALSE(json_has(*main, "provider"));
    CHECK_FALSE(json_has(*main, "model"));
    CHECK(json_str(imza::find_member(*main, "reasoning_effort")) == "off");
    const imza::JsonValue* builder = imza::find_member(*models, "builder");
    REQUIRE(builder != nullptr);
    CHECK_FALSE(json_has(*builder, "provider"));
    CHECK_FALSE(json_has(*builder, "model"));
    check_effort("builder", "default");
    check_effort("researcher", "low");
    check_effort("basic", "off");
    const imza::JsonValue* skills = imza::find_member(written, "skills");
    REQUIRE(skills != nullptr);
    const imza::JsonValue* global = imza::find_member(*skills, "global");
    REQUIRE(global != nullptr);
    CHECK(global->is_object());
    const imza::JsonValue* projects = imza::find_member(*skills, "projects");
    REQUIRE(projects != nullptr);
    CHECK(projects->is_object());
}

TEST_CASE("save_config leaves no temporary or lock file behind")
{
    const auto path = temp_file("notmp.json");
    imza::Config cfg;
    CHECK(imza::save_config(path, cfg) == imza::Status::OK);
    CHECK_FALSE(std::filesystem::exists(path.string() + ".tmp"));
    CHECK_FALSE(std::filesystem::exists(path.string() + ".lock"));
    CHECK_FALSE(imza::test::read_all(path).empty());
}

TEST_CASE("concurrent config updates preserve unrelated changes")
{
    const auto path = temp_file("concurrent.json");
    REQUIRE(imza::save_config(path, imza::Config { }) == imza::Status::OK);

    constexpr int count = 8;
    std::vector<imza::ConfigUpdateResult> results(
        count, imza::ConfigUpdateResult::FAILURE);
    std::vector<std::thread> workers;
    workers.reserve(count);
    for (int index = 0; index < count; ++index) {
        workers.emplace_back([&, index] {
            results[index] = imza::update_config(
                path, imza::Config { }, [index](imza::Config& config) {
                    config.global_skills["skill-" + std::to_string(index)]
                        = imza::SkillPolicy::ALLOW;
                    return true;
                });
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    for (const imza::ConfigUpdateResult result : results) {
        CHECK(result == imza::ConfigUpdateResult::UPDATED);
    }

    imza::Config loaded;
    REQUIRE(imza::load_config(path, loaded) == imza::Status::OK);
    CHECK(loaded.global_skills.size() == count);
}

TEST_CASE("config roundtrip preserves global and project skill policies")
{
    const auto path = temp_file("skills.json");
    imza::Config cfg;
    cfg.global_skills["docs"]                      = imza::SkillPolicy::ALLOW;
    cfg.global_skills["deploy"]                    = imza::SkillPolicy::DENY;
    cfg.project_skills["/work/project"]["release"] = imza::SkillPolicy::ASK;
    REQUIRE(imza::save_config(path, cfg) == imza::Status::OK);
    imza::Config loaded;
    REQUIRE(imza::load_config(path, loaded) == imza::Status::OK);
    CHECK(loaded.global_skills.at("docs") == imza::SkillPolicy::ALLOW);
    CHECK(loaded.global_skills.at("deploy") == imza::SkillPolicy::DENY);
    CHECK(loaded.project_skills.at("/work/project").at("release")
        == imza::SkillPolicy::ASK);
}

TEST_CASE("config roundtrip preserves subagent models")
{
    const auto path = temp_file("subagents.json");
    imza::Config cfg;
    imza::Connection connection;
    connection.id = "openai";
    cfg.providers.push_back(std::move(connection));
    cfg.subagents[imza::SubagentRole::BUILDER]
        = { "openai", "gpt-builder", "" };
    cfg.subagents[imza::SubagentRole::RESEARCH]
        = { "openai", "gpt-research", "high" };
    cfg.subagents[imza::SubagentRole::BASIC] = { "openai", "gpt-basic", "low" };

    REQUIRE(imza::save_config(path, cfg) == imza::Status::OK);
    imza::Config loaded;
    REQUIRE(imza::load_config(path, loaded) == imza::Status::OK);
    CHECK(loaded.subagents.at(imza::SubagentRole::BUILDER).model
        == "gpt-builder");
    CHECK(loaded.subagents.at(imza::SubagentRole::RESEARCH).variant == "high");
    CHECK(loaded.subagents.at(imza::SubagentRole::BASIC).variant == "low");
    const std::string json = imza::test::read_all(path);
    CHECK(json.find("\"models\"") != std::string::npos);
    CHECK(json.find("\"researcher\"") != std::string::npos);
    CHECK(json.find("\"subagents\"") == std::string::npos);
}

TEST_CASE("config preserves main reasoning before a model is selected")
{
    const auto path = temp_file("reasoning-only.json");
    imza::Config cfg;
    cfg.reasoning_effort = "low";
    REQUIRE(imza::save_config(path, cfg) == imza::Status::OK);
    imza::Config loaded;
    REQUIRE(imza::load_config(path, loaded) == imza::Status::OK);
    CHECK_FALSE(loaded.last_used.has_value());
    CHECK(loaded.reasoning_effort == "low");
}

TEST_CASE("config preserves default model with a variant override")
{
    const auto path = temp_file("default-subagent.json");
    imza::Config cfg;
    cfg.subagents[imza::SubagentRole::RESEARCH] = { "", "", "high" };
    REQUIRE(imza::save_config(path, cfg) == imza::Status::OK);
    imza::Config loaded;
    REQUIRE(imza::load_config(path, loaded) == imza::Status::OK);
    const auto& research = loaded.subagents.at(imza::SubagentRole::RESEARCH);
    CHECK(research.provider.empty());
    CHECK(research.model.empty());
    CHECK(research.variant == "high");
}

TEST_CASE("load_config rejects invalid nested configuration values")
{
    struct InvalidConfigCase {
        const char* name;
        const char* filename;
        const char* json;
    };
    const std::array cases {
        InvalidConfigCase { "subagent with unknown connection",
            "invalid-subagents.json",
            R"({"providers": [], "models": {"basic": {
            "provider": "missing", "model": "small", "reasoning_effort": "low"}}})" },
        InvalidConfigCase { "invalid subagent variant",
            "invalid-subagent-variant.json",
            R"({"providers": [{"id": "p"}],
            "models": {"researcher": {"provider": "p", "model": "small",
            "reasoning_effort": "maximum"}}})" },
        InvalidConfigCase { "invalid skill policy", "bad-skills.json",
            R"({"skills":{"global":{"x":"maybe"}}})" },
    };

    for (const auto& invalid : cases) {
        CAPTURE(invalid.name);
        const auto path = temp_file(invalid.filename);
        {
            std::ofstream out(path);
            out << invalid.json;
        }
        imza::Config cfg;
        CHECK(imza::load_config(path, cfg) == imza::Status::CONFIG_ERROR);
    }
}

TEST_CASE("config roundtrip preserves mcp servers")
{
    const auto path = temp_file("mcp-roundtrip.json");
    imza::Config cfg;
    imza::McpServerConfig server;
    server.label                = "Exa";
    server.description          = "Web search";
    server.url                  = "https://mcp.exa.ai/mcp";
    server.headers["x-api-key"] = "secret";
    server.bearer_token         = "tok";
    cfg.mcp_servers["exa"]      = server;

    REQUIRE(imza::save_config(path, cfg) == imza::Status::OK);

    imza::Config loaded;
    REQUIRE(imza::load_config(path, loaded) == imza::Status::OK);
    REQUIRE(loaded.mcp_servers.size() == 1);
    const imza::McpServerConfig& back = loaded.mcp_servers.at("exa");
    CHECK(back.id == "exa");
    CHECK(back.label == "Exa");
    CHECK(back.description == "Web search");
    CHECK(back.url == "https://mcp.exa.ai/mcp");
    CHECK(back.headers.at("x-api-key") == "secret");
    CHECK(back.bearer_token == "tok");
    CHECK(back.enabled);
}

TEST_CASE("load_config validates mcp server entries")
{
    imza::Config cfg;
    std::string error;

    const auto bad_url = temp_file("mcp-bad-url.json");
    {
        std::ofstream out(bad_url);
        out << R"({"mcp_servers":{"exa":{"url":"ftp://x"}}})";
    }
    CHECK(
        imza::load_config(bad_url, cfg, &error) == imza::Status::CONFIG_ERROR);
    CHECK(error.find("http(s) url") != std::string::npos);

    const auto bad_header = temp_file("mcp-bad-header.json");
    {
        std::ofstream out(bad_header);
        out << R"({"mcp_servers":{"exa":{"url":"https://x",)"
            << R"("headers":{"Bad:Name":"v"}}}})";
    }
    CHECK(imza::load_config(bad_header, cfg, &error)
        == imza::Status::CONFIG_ERROR);
    CHECK(error.find("invalid header name") != std::string::npos);

    const auto upgrade = temp_file("mcp-http-upgrade.json");
    {
        std::ofstream out(upgrade);
        out << R"({"mcp_servers":{"exa":{"url":"http://mcp.exa.ai/mcp"}}})";
    }
    REQUIRE(imza::load_config(upgrade, cfg) == imza::Status::OK);
    CHECK(cfg.mcp_servers.at("exa").url == "https://mcp.exa.ai/mcp");
    CHECK(cfg.mcp_servers.at("exa").enabled);
}

TEST_CASE("mcp config accepts catalogue references without urls")
{
    const auto path = temp_file("mcp-catalog-ref.json");
    {
        std::ofstream out(path);
        out << R"({"mcp_servers":{"ctx":{"catalog_id":"context7",)"
            << R"("bearer_token":"tok"}}})";
    }
    imza::Config cfg;
    REQUIRE(imza::load_config(path, cfg) == imza::Status::OK);
    const auto& server = cfg.mcp_servers.at("ctx");
    CHECK(server.catalog_id == "context7");
    CHECK(server.url.empty());

    REQUIRE(imza::save_config(path, cfg) == imza::Status::OK);
    imza::Config loaded;
    REQUIRE(imza::load_config(path, loaded) == imza::Status::OK);
    CHECK(loaded.mcp_servers.at("ctx").catalog_id == "context7");
    CHECK(loaded.mcp_servers.at("ctx").url.empty());

    const auto neither = temp_file("mcp-neither.json");
    {
        std::ofstream out(neither);
        out << R"({"mcp_servers":{"broken":{"label":"x"}}})";
    }
    imza::Config bad;
    std::string error;
    CHECK(
        imza::load_config(neither, bad, &error) == imza::Status::CONFIG_ERROR);
    CHECK(error.find("url or a catalog id") != std::string::npos);
}
TEST_CASE("config roundtrips a stdio mcp server")
{
    const auto path = temp_file("mcp-stdio-roundtrip.json");
    imza::Config cfg;
    imza::McpServerConfig server;
    server.id           = "local";
    server.type         = "stdio";
    server.command      = "npx";
    server.args         = { "-y", "@modelcontextprotocol/server-filesystem" };
    server.env["TOKEN"] = "abc";
    server.working_directory = "/tmp";
    cfg.mcp_servers["local"] = server;

    REQUIRE(imza::save_config(path, cfg) == imza::Status::OK);
    imza::Config loaded;
    REQUIRE(imza::load_config(path, loaded) == imza::Status::OK);
    const imza::McpServerConfig& back = loaded.mcp_servers.at("local");
    CHECK(back.type == "stdio");
    CHECK(back.is_stdio());
    CHECK(back.command == "npx");
    REQUIRE(back.args.size() == 2);
    CHECK(back.args[1] == "@modelcontextprotocol/server-filesystem");
    CHECK(back.env.at("TOKEN") == "abc");
    CHECK(back.working_directory == "/tmp");
    CHECK(back.url.empty());
}

TEST_CASE("load_config validates stdio mcp server entries")
{
    const auto no_command = temp_file("mcp-stdio-no-command.json");
    {
        std::ofstream out(no_command);
        out << R"({"mcp_servers":{"local":{"type":"stdio"}}})";
    }
    imza::Config cfg;
    std::string error;
    CHECK(imza::load_config(no_command, cfg, &error)
        == imza::Status::CONFIG_ERROR);
    CHECK(error.find("requires a command") != std::string::npos);

    const auto bad_type = temp_file("mcp-bad-type.json");
    {
        std::ofstream out(bad_type);
        out << R"({"mcp_servers":{"x":{"type":"carrier-pigeon",)"
            << R"("url":"https://x"}}})";
    }
    imza::Config typed;
    CHECK(imza::load_config(bad_type, typed, &error)
        == imza::Status::CONFIG_ERROR);
    CHECK(error.find("unknown type") != std::string::npos);

    // An absent type defaults to http.
    const auto defaulted = temp_file("mcp-default-type.json");
    {
        std::ofstream out(defaulted);
        out << R"({"mcp_servers":{"x":{"url":"https://x"}}})";
    }
    imza::Config plain;
    REQUIRE(imza::load_config(defaulted, plain) == imza::Status::OK);
    CHECK(plain.mcp_servers.at("x").type == "http");
    CHECK_FALSE(plain.mcp_servers.at("x").is_stdio());
}
TEST_CASE("config roundtrip preserves the allow list and instructions")
{
    const auto path = temp_file("allow-roundtrip.json");
    imza::Config cfg;
    cfg.allow.directories.emplace_back("/work/project");
    cfg.allow.commands.push_back(imza::ShellCommandGrant { "git", "push" });
    cfg.allow.commands.push_back(
        imza::ShellCommandGrant { "rg", std::nullopt });
    cfg.instructions.push_back("docs/guide.md");

    REQUIRE(imza::save_config(path, cfg) == imza::Status::OK);

    imza::Config loaded;
    REQUIRE(imza::load_config(path, loaded) == imza::Status::OK);
    REQUIRE(loaded.allow.directories.size() == 1);
    CHECK(
        loaded.allow.directories[0] == std::filesystem::path("/work/project"));
    REQUIRE(loaded.allow.commands.size() == 2);
    CHECK(loaded.allow.commands[0].program == "git");
    CHECK(loaded.allow.commands[0].subcommand == "push");
    CHECK(loaded.allow.commands[1].program == "rg");
    CHECK_FALSE(loaded.allow.commands[1].subcommand.has_value());
    REQUIRE(loaded.instructions.size() == 1);
    CHECK(loaded.instructions[0] == "docs/guide.md");
}

TEST_CASE("allow grants skip directories that no longer exist")
{
    const imza::test::TempDir dir;
    imza::AllowConfig allow;
    allow.directories.emplace_back(dir.path);
    allow.directories.emplace_back(dir.path / "gone");
    allow.commands.push_back(imza::ShellCommandGrant { "git", "push" });

    const imza::PermissionStore::Grants grants = imza::allow_grants(allow);
    REQUIRE(grants.size() == 2);
    CHECK(std::holds_alternative<imza::ExternalGrant>(grants[0]));
    CHECK(std::get<imza::ExternalGrant>(grants[0])
        == std::filesystem::weakly_canonical(dir.path));
    const imza::ShellCommandGrant& command
        = std::get<imza::ShellCommandGrant>(grants[1]);
    CHECK(command.program == "git");
    CHECK(command.subcommand == "push");
}

TEST_CASE("allow grants resolve relative directories against the cwd")
{
    const imza::test::CurrentDirectory cwd;
    const imza::test::TempDir base;
    REQUIRE(std::filesystem::create_directories(base.path / "sub"));
    std::error_code error;
    std::filesystem::current_path(base.path, error);
    REQUIRE_FALSE(error);

    imza::AllowConfig allow;
    allow.directories.emplace_back("sub");
    const imza::PermissionStore::Grants grants = imza::allow_grants(allow);
    REQUIRE(grants.size() == 1);
    CHECK(std::get<imza::ExternalGrant>(grants[0])
        == std::filesystem::weakly_canonical(base.path / "sub"));
}

TEST_CASE("configured allow grants install into the permission store")
{
    const imza::test::TempDir dir;
    imza::Config cfg;
    cfg.allow.directories.emplace_back(dir.path);
    cfg.allow.commands.push_back(imza::ShellCommandGrant { "git", "push" });

    imza::PermissionStore store;
    REQUIRE(store.install(imza::allow_grants(cfg.allow)));
    const imza::PermissionStore::Snapshot grants = store.snapshot();
    CHECK(
        imza::grants_cover(grants, imza::ShellCommandGrant { "git", "push" }));
    CHECK(
        imza::grants_cover(grants, imza::ShellCommandGrant { "git", "status" })
        == false);
    CHECK(imza::grants_cover(
        grants, std::filesystem::weakly_canonical(dir.path / "inner")));
}

TEST_CASE("load_config validates allow list and instruction entries")
{
    struct InvalidConfigCase {
        const char* name;
        const char* filename;
        const char* json;
    };
    const std::array cases {
        InvalidConfigCase { "command without program", "allow-no-program.json",
            R"({"allow":{"commands":[{"subcommand":"push"}]}})" },
        InvalidConfigCase { "command with empty program",
            "allow-empty-program.json",
            R"({"allow":{"commands":[{"program":""}]}})" },
        InvalidConfigCase { "command with empty subcommand",
            "allow-empty-subcommand.json",
            R"({"allow":{"commands":[{"program":"git","subcommand":""}]}})" },
        InvalidConfigCase { "empty directory", "allow-empty-dir.json",
            R"({"allow":{"directories":[""]}})" },
        InvalidConfigCase { "empty instruction", "empty-instruction.json",
            R"({"instructions":["docs/a.md",""]})" },
    };

    for (const auto& invalid : cases) {
        CAPTURE(invalid.name);
        const auto path = temp_file(invalid.filename);
        {
            std::ofstream out(path);
            out << invalid.json;
        }
        imza::Config cfg;
        std::string error;
        CHECK(
            imza::load_config(path, cfg, &error) == imza::Status::CONFIG_ERROR);
        CHECK_FALSE(error.empty());
    }
}

TEST_CASE("allow and instructions are absent from a minimal config")
{
    const auto path = temp_file("no-allow.json");
    REQUIRE(imza::save_config(path, imza::Config { }) == imza::Status::OK);
    const std::string json = imza::test::read_all(path);
    CHECK(json.find("\"allow\"") == std::string::npos);
    CHECK(json.find("\"instructions\"") == std::string::npos);

    imza::Config loaded;
    REQUIRE(imza::load_config(path, loaded) == imza::Status::OK);
    CHECK(loaded.allow.directories.empty());
    CHECK(loaded.allow.commands.empty());
    CHECK(loaded.instructions.empty());
}
