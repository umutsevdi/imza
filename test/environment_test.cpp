#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>

#include "subsystems/environment.h"

namespace {

TEST_CASE("git status parser creates typed changed files")
{
    using Kind = ursa::ChangedFile::Kind;
    const auto files
        = ursa::parse_git_status(" M src/app.cpp\n"
                                 "A  new file.cpp\n"
                                 "?? notes.txt\n"
                                 "D  old.cpp\n"
                                 "R  old name.cpp -> new name.cpp\n"
                                 "C  source.cpp -> copy.cpp\n"
                                 "UU conflict.cpp\n"
                                 " T type.cpp\r\n"
                                 "!! ignored.cpp\n"
                                 "malformed\n");

    REQUIRE(files.size() == 9);
    CHECK((files[0] == ursa::ChangedFile { "src/app.cpp", Kind::MODIFIED }));
    CHECK((files[1] == ursa::ChangedFile { "new file.cpp", Kind::ADDED }));
    CHECK((files[2] == ursa::ChangedFile { "notes.txt", Kind::UNTRACKED }));
    CHECK((files[3] == ursa::ChangedFile { "old.cpp", Kind::DELETED }));
    CHECK((files[4]
        == ursa::ChangedFile {
            "old name.cpp -> new name.cpp", Kind::RENAMED }));
    CHECK((files[5]
        == ursa::ChangedFile { "source.cpp -> copy.cpp", Kind::COPIED }));
    CHECK((files[6] == ursa::ChangedFile { "conflict.cpp", Kind::CONFLICTED }));
    CHECK((files[7] == ursa::ChangedFile { "type.cpp", Kind::MODIFIED }));
    CHECK((files[8] == ursa::ChangedFile { "ignored.cpp", Kind::UNKNOWN }));
}

TEST_CASE("git status parser resolves combined index and worktree states")
{
    using Kind = ursa::ChangedFile::Kind;
    const auto files
        = ursa::parse_git_status("AM added.cpp\nMD deleted.cpp\nAA both.cpp\n");

    REQUIRE(files.size() == 3);
    CHECK(files[0].kind == Kind::ADDED);
    CHECK(files[1].kind == Kind::DELETED);
    CHECK(files[2].kind == Kind::CONFLICTED);
}

TEST_CASE("git branch normalization removes command whitespace")
{
    CHECK(ursa::normalize_git_branch("feature/status-ui\n")
        == "feature/status-ui");
    CHECK(ursa::normalize_git_branch("main\r\n") == "main");
    CHECK(ursa::normalize_git_branch("").empty());
}

TEST_CASE("git diff summary counts lines and fingerprints content")
{
    const auto first
        = ursa::summarize_git_diff("2\t1\tfile.cpp\n"
                                   "-\t-\timage.png\n\n"
                                   "diff --git a/file.cpp b/file.cpp\n"
                                   "-old\n+new\n+more\n");
    const auto second
        = ursa::summarize_git_diff("2\t1\tfile.cpp\n"
                                   "-\t-\timage.png\n\n"
                                   "diff --git a/file.cpp b/file.cpp\n"
                                   "-old\n+next\n+more\n");

    CHECK(first.additions == 2);
    CHECK(first.deletions == 1);
    CHECK(first.signature != second.signature);
}

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
    ursa::Environment env;
    const auto sys = env.system();
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
    ursa::Environment env;
    REQUIRE(wait_until_ready(env));
    CHECK(env.ready());
}

TEST_CASE("workspace is null outside a project while environment is ready")
{
    const auto original = std::filesystem::current_path();
    const auto dir      = original.root_path();

    ursa::Environment env;
    REQUIRE(wait_until_ready(env));
    REQUIRE(env.chdir(dir));
    CHECK(env.ready());
    CHECK(env.workspace() == nullptr);
    REQUIRE(env.repository() != nullptr);
    CHECK(env.repository()->branch.empty());
    CHECK(env.repository()->changed_files.empty());

    std::filesystem::current_path(original);
}

TEST_CASE("workspace subscription fires on readiness")
{
    ursa::Environment env;
    std::shared_ptr<const ursa::WorkspaceEnvironment> captured;
    auto subscription = env.subscribe_to_workspace_change(
        [&] { captured = env.workspace(); });
    REQUIRE(wait_until_ready(env));
    CHECK(env.ready());
    CHECK(captured != nullptr);
    CHECK(captured == env.workspace());
}

TEST_CASE("workspace carries an instruction and project skills when rooted")
{
    const auto original = std::filesystem::current_path();
    const auto root
        = std::filesystem::temp_directory_path() / "ursa_test_wsroot";
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
