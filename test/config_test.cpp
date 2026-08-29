#include <doctest/doctest.h>
#include <json/json.h>

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "types.h"

namespace {

std::filesystem::path temp_file(const std::string& name)
{
    static int counter = 0;
    auto dir = std::filesystem::temp_directory_path()
        / ("ursa-config-test-" + std::to_string(::getpid()) + "-"
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
    ursa::Config cfg;
    std::string error;
    CHECK(ursa::load_config(path, cfg, &error) == ursa::Status::OK);
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
    ursa::Config cfg;
    std::string error;
    CHECK(ursa::load_config(path, cfg, &error) == ursa::Status::CONFIG_ERROR);
    CHECK_FALSE(error.empty());
}

TEST_CASE("load_config legacy-less body without providers is empty")
{
    const auto path = temp_file("empty.json");
    {
        std::ofstream out(path);
        out << "{\"other\": 1}";
    }
    ursa::Config cfg;
    CHECK(ursa::load_config(path, cfg) == ursa::Status::OK);
    CHECK(cfg.providers.empty());
}

TEST_CASE("config roundtrip preserves connections and last_used")
{
    const auto path = temp_file("roundtrip.json");
    ursa::Config cfg;
    ursa::Connection conn;
    conn.id          = "openrouter";
    conn.provider_id = "openrouter";
    conn.api_key     = "sk-or-test";
    cfg.providers.push_back(conn);

    ursa::Connection local;
    local.id          = "local";
    local.provider_id = "local";
    local.endpoint    = "http://localhost:11434/v1/chat/completions";
    local.api_key     = "";
    local.dialects["glm-5.3"] = ursa::ApiStandard::ANTHROPIC;
    cfg.providers.push_back(local);

    cfg.last_used = ursa::LastUsed { "openrouter", "" };

    REQUIRE(ursa::save_config(path, cfg) == ursa::Status::OK);

    ursa::Config loaded;
    CHECK(ursa::load_config(path, loaded) == ursa::Status::OK);
    REQUIRE(loaded.providers.size() == 2);
    CHECK(loaded.providers[0].id == "openrouter");
    CHECK(loaded.providers[0].provider_id == "openrouter");
    CHECK(loaded.providers[0].api_key == "sk-or-test");
    CHECK(loaded.providers[0].endpoint.empty());
    CHECK(loaded.providers[1].endpoint
        == "http://localhost:11434/v1/chat/completions");
    REQUIRE(loaded.providers[1].dialects.count("glm-5.3") == 1);
    CHECK(loaded.providers[1].dialects.at("glm-5.3")
        == ursa::ApiStandard::ANTHROPIC);
    REQUIRE(loaded.last_used.has_value());
    CHECK(loaded.last_used->provider == "openrouter");
    CHECK(loaded.last_used->model.empty());
}

TEST_CASE("load_config rejects providers without id or provider_id")
{
    const auto path = temp_file("invalid.json");
    {
        std::ofstream out(path);
        out << R"({"providers": [{"id": "", "provider_id": "openai"}]})";
    }
    ursa::Config cfg;
    CHECK(ursa::load_config(path, cfg) == ursa::Status::CONFIG_ERROR);
}

TEST_CASE("load_config rejects unknown dialect")
{
    const auto path = temp_file("dialect.json");
    {
        std::ofstream out(path);
        out << R"({"providers": [{"id": "a", "provider_id": "openai",
            "dialects": {"m": "grpc"}}]})";
    }
    ursa::Config cfg;
    CHECK(ursa::load_config(path, cfg) == ursa::Status::CONFIG_ERROR);
}

TEST_CASE("load_config rejects unresolved last_used.provider")
{
    const auto path = temp_file("last.json");
    {
        std::ofstream out(path);
        out << R"({"providers": [], "last_used": {"provider": "x",
            "model": "m"}})";
    }
    ursa::Config cfg;
    CHECK(ursa::load_config(path, cfg) == ursa::Status::CONFIG_ERROR);
}

TEST_CASE("save_config creates parent directories")
{
    auto path = std::filesystem::temp_directory_path()
        / ("ursa-config-nested-" + std::to_string(::getpid()))
        / "deep" / "nested" / "config.json";
    std::filesystem::remove_all(path.parent_path().parent_path());

    ursa::Config cfg;
    CHECK(ursa::save_config(path, cfg) == ursa::Status::OK);
    CHECK(std::filesystem::exists(path));

    ursa::Config loaded;
    CHECK(ursa::load_config(path, loaded) == ursa::Status::OK);
    CHECK(loaded.providers.empty());
    std::filesystem::remove_all(path.parent_path().parent_path());
}

TEST_CASE("save_config leaves no temp file behind")
{
    const auto path = temp_file("notmp.json");
    ursa::Config cfg;
    CHECK(ursa::save_config(path, cfg) == ursa::Status::OK);
    CHECK_FALSE(std::filesystem::exists(path.string() + ".tmp"));
    CHECK_FALSE(read_all(path).empty());
}

TEST_CASE("config roundtrip preserves global and project skill policies")
{
    const auto path = temp_file("skills.json");
    ursa::Config cfg;
    cfg.global_skills["docs"] = ursa::SkillPolicy::ALLOW;
    cfg.global_skills["deploy"] = ursa::SkillPolicy::DENY;
    cfg.project_skills["/work/project"]["release"] = ursa::SkillPolicy::ASK;
    REQUIRE(ursa::save_config(path, cfg) == ursa::Status::OK);
    ursa::Config loaded;
    REQUIRE(ursa::load_config(path, loaded) == ursa::Status::OK);
    CHECK(loaded.global_skills.at("docs") == ursa::SkillPolicy::ALLOW);
    CHECK(loaded.global_skills.at("deploy") == ursa::SkillPolicy::DENY);
    CHECK(loaded.project_skills.at("/work/project").at("release")
        == ursa::SkillPolicy::ASK);
}

TEST_CASE("config rejects invalid skill policies")
{
    const auto path = temp_file("bad-skills.json");
    { std::ofstream out(path); out << R"({"skills":{"global":{"x":"maybe"}}})"; }
    ursa::Config cfg;
    CHECK(ursa::load_config(path, cfg) == ursa::Status::CONFIG_ERROR);
}
