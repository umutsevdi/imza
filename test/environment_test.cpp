#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>

#include "environment.h"

namespace {

    void write_file(const std::filesystem::path& path, std::string_view content)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << content;
    }

    bool wait_until_ready(const ursa::Environment& env, int timeout_ms = 5000)
    {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (env.ready()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return env.ready();
    }

    TEST_CASE("system environment populates the core fields synchronously")
    {
        const auto env = ursa::get_environment();
        const auto sys = env->system();
        REQUIRE(sys != nullptr);
        CHECK_FALSE(sys->os_name.empty());
        CHECK_FALSE(sys->default_shell.empty());
        CHECK_FALSE(sys->today.empty());
        CHECK(sys->today.size() == 10);
        CHECK(sys->today[4] == '-');
        CHECK(sys->today[7] == '-');
    }

    TEST_CASE("environment becomes ready after the workspace scan")
    {
        const auto env = ursa::get_environment();
        REQUIRE(wait_until_ready(*env));
        CHECK(env->ready());
    }

    TEST_CASE("workspace may be null in a non-git folder while still ready")
    {
        const auto original = std::filesystem::current_path();
        const auto dir = std::filesystem::temp_directory_path()
            / "ursa_test_nongit";
        std::filesystem::remove_all(dir);
        REQUIRE(std::filesystem::create_directories(dir));

        ursa::Environment env;
        REQUIRE(wait_until_ready(env));
        REQUIRE(env.chdir(dir));
        CHECK(env.ready());
        REQUIRE(env.workspace() != nullptr);
        CHECK_FALSE(env.workspace()->project_root.has_value());
        CHECK_FALSE(env.agent_rules_path().has_value());
        CHECK(env.project_skills() == 0);

        std::filesystem::current_path(original);
        std::filesystem::remove_all(dir);
    }

    TEST_CASE("workspace subscription fires on readiness")
    {
        ursa::Environment env;
        std::shared_ptr<const ursa::WorkspaceEnvironment> captured;
        env.subscribe_to_workspace_change(
            [&](std::shared_ptr<const ursa::WorkspaceEnvironment> ws) {
                captured = std::move(ws);
            });
        REQUIRE(wait_until_ready(env));
        CHECK(env.ready());
        CHECK(captured != nullptr);
        CHECK(captured == env.workspace());
    }

    TEST_CASE("workspace carries an instruction and project skills when rooted")
    {
        const auto original = std::filesystem::current_path();
        const auto root = std::filesystem::temp_directory_path()
            / "ursa_test_wsroot";
        std::filesystem::remove_all(root);
        const auto git = root / ".git";
        REQUIRE(std::filesystem::create_directories(git));
        write_file(root / "AGENTS.md", "agents rules");

        ursa::Environment env;
        REQUIRE(wait_until_ready(env));
        REQUIRE(env.chdir(root));
        const auto ws = env.workspace();
        REQUIRE(ws != nullptr);
        REQUIRE(ws->project_root.has_value());
        REQUIRE(ws->instruction.has_value());
        CHECK(ws->instruction->content == "agents rules");
        CHECK(env.agent_rules_path() == "AGENTS.md");

        std::filesystem::current_path(original);
        std::filesystem::remove_all(root);
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
