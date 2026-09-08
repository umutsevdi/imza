#include <doctest/doctest.h>
#include <json/json.h>

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include "platform/config.h"

namespace {

std::filesystem::path temp_file(const std::string& name)
{
    static int counter = 0;
    auto dir           = std::filesystem::temp_directory_path()
        / ("imza-config-test-" + std::to_string(::getpid()) + "-"
            + std::to_string(counter++));
    std::filesystem::create_directories(dir);
    return dir / name;
}

std::string read_all(const std::filesystem::path& path)
{
    std::ifstream file(path);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

} // namespace

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
    conn.id          = "openrouter";
    conn.provider_id = "openrouter";
    conn.api_key     = "sk-or-test";
    cfg.providers.push_back(conn);

    imza::Connection local;
    local.id                  = "local";
    local.provider_id         = "local";
    local.endpoint            = "http://localhost:11434/v1/chat/completions";
    local.api_key             = "";
    local.label               = "my Ollama";
    local.dialects["glm-5.3"] = imza::ApiStandard::ANTHROPIC;
    cfg.providers.push_back(local);

    cfg.last_used        = imza::LastUsed { "openrouter", "" };
    cfg.reasoning_effort = "high";

    REQUIRE(imza::save_config(path, cfg) == imza::Status::OK);

    imza::Config loaded;
    CHECK(imza::load_config(path, loaded) == imza::Status::OK);
    REQUIRE(loaded.providers.size() == 2);
    CHECK(loaded.providers[0].id == "openrouter");
    CHECK(loaded.providers[0].provider_id == "openrouter");
    CHECK(loaded.providers[0].api_key == "sk-or-test");
    CHECK(loaded.providers[0].endpoint.empty());
    CHECK(loaded.providers[0].label.empty());
    CHECK(loaded.providers[1].label == "my Ollama");
    CHECK(loaded.providers[1].endpoint
        == "http://localhost:11434/v1/chat/completions");
    REQUIRE(loaded.providers[1].dialects.count("glm-5.3") == 1);
    CHECK(loaded.providers[1].dialects.at("glm-5.3")
        == imza::ApiStandard::ANTHROPIC);
    REQUIRE(loaded.last_used.has_value());
    CHECK(loaded.last_used->provider == "openrouter");
    CHECK(loaded.last_used->model.empty());
    CHECK(loaded.reasoning_effort == "high");
    Json::Value written;
    Json::CharReaderBuilder reader;
    std::string errors;
    std::istringstream stream(read_all(path));
    REQUIRE(Json::parseFromStream(reader, stream, &written, &errors));
    CHECK(written["providers"][0]["id"] == "openrouter");
    CHECK(written["providers"][0]["endpoint"] == "");
    CHECK(written["providers"][0]["label"] == "");
    CHECK_FALSE(written["providers"][0].isMember("dialects"));
    CHECK(written["providers"][1]["label"] == "my Ollama");
    CHECK(written["models"]["main"]["provider"] == "openrouter");
    CHECK(written["models"]["main"]["reasoning_effort"] == "high");
    CHECK_FALSE(written.isMember("last_used"));
    CHECK_FALSE(written.isMember("reasoning_effort"));
}

TEST_CASE("load_config rejects providers without id or provider_id")
{
    const auto path = temp_file("invalid.json");
    {
        std::ofstream out(path);
        out << R"({"providers": [{"id": "", "provider_id": "openai"}]})";
    }
    imza::Config cfg;
    CHECK(imza::load_config(path, cfg) == imza::Status::CONFIG_ERROR);
}

TEST_CASE("load_config rejects unknown dialect")
{
    const auto path = temp_file("dialect.json");
    {
        std::ofstream out(path);
        out << R"({"providers": [{"id": "a", "provider_id": "openai",
            "dialects": {"m": "grpc"}}]})";
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

    Json::Value written;
    Json::CharReaderBuilder reader;
    std::string errors;
    std::istringstream stream(read_all(path));
    REQUIRE(Json::parseFromStream(reader, stream, &written, &errors));
    CHECK(written["providers"].isArray());
    CHECK_FALSE(written["models"]["main"].isMember("provider"));
    CHECK_FALSE(written["models"]["main"].isMember("model"));
    CHECK(written["models"]["main"]["reasoning_effort"] == "off");
    CHECK_FALSE(written["models"]["builder"].isMember("provider"));
    CHECK_FALSE(written["models"]["builder"].isMember("model"));
    CHECK(written["models"]["builder"]["reasoning_effort"] == "default");
    CHECK(written["models"]["researcher"]["reasoning_effort"] == "low");
    CHECK(written["models"]["basic"]["reasoning_effort"] == "off");
    CHECK(written["skills"]["global"].isObject());
    CHECK(written["skills"]["projects"].isObject());
}

TEST_CASE("save_config leaves no temp file behind and keeps the lock file")
{
    const auto path = temp_file("notmp.json");
    imza::Config cfg;
    CHECK(imza::save_config(path, cfg) == imza::Status::OK);
    CHECK_FALSE(std::filesystem::exists(path.string() + ".tmp"));
    CHECK(std::filesystem::exists(path.string() + ".lock"));
    CHECK_FALSE(read_all(path).empty());
}

TEST_CASE("concurrent config updates preserve unrelated changes")
{
    const auto path = temp_file("concurrent.json");
    REQUIRE(imza::save_config(path, imza::Config { }) == imza::Status::OK);

    constexpr int count = 8;
    std::vector<imza::ConfigUpdateResult> results(
        count, imza::ConfigUpdateResult::ERROR);
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
    cfg.providers.push_back(imza::Connection { "openai", "openai", "", "" });
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
    const std::string json = read_all(path);
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

TEST_CASE("load_config rejects subagent with unknown connection")
{
    const auto path = temp_file("invalid-subagents.json");
    {
        std::ofstream out(path);
        out << R"({"providers": [], "models": {"basic": {
            "provider": "missing", "model": "small", "reasoning_effort": "low"}}})";
    }
    imza::Config cfg;
    CHECK(imza::load_config(path, cfg) == imza::Status::CONFIG_ERROR);
}

TEST_CASE("load_config rejects invalid subagent variant")
{
    const auto path = temp_file("invalid-subagent-variant.json");
    {
        std::ofstream out(path);
        out << R"({"providers": [{"id": "p", "provider_id": "openai"}],
            "models": {"researcher": {"provider": "p", "model": "small",
            "reasoning_effort": "maximum"}}})";
    }
    imza::Config cfg;
    CHECK(imza::load_config(path, cfg) == imza::Status::CONFIG_ERROR);
}

TEST_CASE("config rejects invalid skill policies")
{
    const auto path = temp_file("bad-skills.json");
    {
        std::ofstream out(path);
        out << R"({"skills":{"global":{"x":"maybe"}}})";
    }
    imza::Config cfg;
    CHECK(imza::load_config(path, cfg) == imza::Status::CONFIG_ERROR);
}
