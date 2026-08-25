#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <string_view>

#include "environment.h"

namespace {

    void write_file(const std::filesystem::path& path, std::string_view content)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << content;
    }

    TEST_CASE("analyze_environment populates the core fields")
    {
        const auto e = ursa::analyze_environment();
        CHECK_FALSE(e.os_name.empty());
        CHECK_FALSE(e.default_shell.empty());
        CHECK_FALSE(e.today.empty());
        CHECK(e.today.size() == 10);
        CHECK(e.today[4] == '-');
        CHECK(e.today[7] == '-');
    }

    TEST_CASE("analyze_environment_async resolves to a ready future")
    {
        auto future = ursa::analyze_environment_async();
        REQUIRE(future.valid());
        CHECK(future.wait_for(std::chrono::seconds(5))
            == std::future_status::ready);
        const auto e = future.get();
        CHECK_FALSE(e.os_name.empty());
    }

    TEST_CASE("load_agent_file prefers AGENTS.md over other candidates")
    {
        const auto dir
            = std::filesystem::temp_directory_path() / "ursa_test_agents_pre";
        std::filesystem::remove_all(dir);
        REQUIRE(std::filesystem::create_directories(dir));
        write_file(dir / "AGENTS.md", "agents rules");
        write_file(dir / "CLAUDE.md", "claude rules");

        const auto found = ursa::load_agent_file(dir);
        REQUIRE(found.has_value());
        CHECK(found->path == "AGENTS.md");
        CHECK(found->content == "agents rules");

        std::filesystem::remove_all(dir);
    }

    TEST_CASE("load_agent_file falls back to the next candidate")
    {
        const auto dir
            = std::filesystem::temp_directory_path() / "ursa_test_agents_fb";
        std::filesystem::remove_all(dir);
        REQUIRE(std::filesystem::create_directories(dir));
        write_file(dir / "GEMINI.md", "gemini rules");

        const auto found = ursa::load_agent_file(dir);
        REQUIRE(found.has_value());
        CHECK(found->path == "GEMINI.md");
        CHECK(found->content == "gemini rules");

        std::filesystem::remove_all(dir);
    }

    TEST_CASE("load_agent_file returns nullopt when no candidate exists")
    {
        const auto dir
            = std::filesystem::temp_directory_path() / "ursa_test_agents_empty";
        std::filesystem::remove_all(dir);
        REQUIRE(std::filesystem::create_directories(dir));

        CHECK_FALSE(ursa::load_agent_file(dir).has_value());

        std::filesystem::remove_all(dir);
    }

    TEST_CASE("load_agent_file truncates oversized content")
    {
        const auto dir
            = std::filesystem::temp_directory_path() / "ursa_test_agents_big";
        std::filesystem::remove_all(dir);
        REQUIRE(std::filesystem::create_directories(dir));
        write_file(dir / "AGENTS.md", std::string(64 * 1024, 'x'));

        const auto found = ursa::load_agent_file(dir);
        REQUIRE(found.has_value());
        CHECK(found->content.find("[truncated]") != std::string::npos);
        CHECK(found->content.size() < 64 * 1024);

        std::filesystem::remove_all(dir);
    }

} // namespace
