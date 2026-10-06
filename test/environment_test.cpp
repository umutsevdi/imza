#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "test_fs.h"
#include "workspace/environment.h"

namespace {

TEST_CASE("git status parser creates typed changed files")
{
    using Kind = imza::ChangedFile::Kind;
    const auto files
        = imza::parse_git_status(" M src/app.cpp\n"
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
    CHECK((files[0] == imza::ChangedFile { "src/app.cpp", Kind::MODIFIED }));
    CHECK((files[1] == imza::ChangedFile { "new file.cpp", Kind::ADDED }));
    CHECK((files[2] == imza::ChangedFile { "notes.txt", Kind::UNTRACKED }));
    CHECK((files[3] == imza::ChangedFile { "old.cpp", Kind::DELETED }));
    CHECK((files[4]
        == imza::ChangedFile {
            "old name.cpp -> new name.cpp", Kind::RENAMED }));
    CHECK((files[5]
        == imza::ChangedFile { "source.cpp -> copy.cpp", Kind::COPIED }));
    CHECK((files[6] == imza::ChangedFile { "conflict.cpp", Kind::CONFLICTED }));
    CHECK((files[7] == imza::ChangedFile { "type.cpp", Kind::MODIFIED }));
    CHECK((files[8] == imza::ChangedFile { "ignored.cpp", Kind::UNKNOWN }));
}

TEST_CASE("git status parser resolves combined index and worktree states")
{
    using Kind = imza::ChangedFile::Kind;
    const auto files
        = imza::parse_git_status("AM added.cpp\nMD deleted.cpp\nAA both.cpp\n");

    REQUIRE(files.size() == 3);
    CHECK(files[0].kind == Kind::ADDED);
    CHECK(files[1].kind == Kind::DELETED);
    CHECK(files[2].kind == Kind::CONFLICTED);
}

TEST_CASE("git branch normalization removes command whitespace")
{
    CHECK(imza::normalize_git_branch("feature/status-ui\n")
        == "feature/status-ui");
    CHECK(imza::normalize_git_branch("main\r\n") == "main");
    CHECK(imza::normalize_git_branch("").empty());
}

TEST_CASE("git diff summary counts lines and fingerprints content")
{
    const auto first
        = imza::summarize_git_diff("2\t1\tfile.cpp\n"
                                   "-\t-\timage.png\n\n"
                                   "diff --git a/file.cpp b/file.cpp\n"
                                   "-old\n+new\n+more\n");
    const auto second
        = imza::summarize_git_diff("2\t1\tfile.cpp\n"
                                   "-\t-\timage.png\n\n"
                                   "diff --git a/file.cpp b/file.cpp\n"
                                   "-old\n+next\n+more\n");

    CHECK(first.additions == 2);
    CHECK(first.deletions == 1);
    CHECK(first.signature != second.signature);
}

bool wait_until_ready(const imza::Environment& env, int timeout_ms = 5000)
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
    imza::Environment env;
    const auto sys = env.system();
    REQUIRE(sys != nullptr);
    CHECK_FALSE(sys->os_name.empty());
    CHECK_FALSE(sys->default_shell.empty());
    CHECK_FALSE(sys->today.empty());
    CHECK(std::filesystem::is_directory(sys->temporary_directory));
    CHECK(sys->temporary_directory.filename() == "imza");
    CHECK(sys->today.size() == 10);
    CHECK(sys->today[4] == '-');
    CHECK(sys->today[7] == '-');
}

TEST_CASE("Imza temporary directory is reusable and canonical")
{
    const imza::test::TempDir base_dir;
    const auto base = base_dir.path;
    std::error_code error;
    std::filesystem::create_directories(base / "real", error);
    REQUIRE_FALSE(error);
#ifdef _WIN32
    const auto input = base / "real";
#else
    std::filesystem::create_directory_symlink(base / "real", base / "link");
    const auto input = base / "link";
#endif

    const auto first  = imza::prepare_imza_temporary_directory(input);
    const auto second = imza::prepare_imza_temporary_directory(input);
    CHECK(first == second);
    CHECK(first == std::filesystem::weakly_canonical(input / "imza"));
    CHECK(std::filesystem::is_directory(first));

    std::filesystem::remove_all(first, error);
    imza::test::write_file(first, "collision");
    CHECK_THROWS_AS(imza::prepare_imza_temporary_directory(input),
        std::filesystem::filesystem_error);
}

TEST_CASE("workspace retains its directory outside a project")
{
    const imza::test::CurrentDirectory directory;
    const auto dir = directory.original.root_path();

    imza::Environment env;
    REQUIRE(wait_until_ready(env));
    REQUIRE(env.chdir(dir) == imza::Environment::ChdirResult::CHANGED);
    CHECK(env.ready());
    REQUIRE(env.workspace() != nullptr);
    CHECK(env.workspace()->working_directory == dir);
    CHECK_FALSE(env.workspace()->project_root.has_value());
    REQUIRE(env.repository() != nullptr);
    CHECK(env.repository()->branch.empty());
    CHECK(env.repository()->changed_files.empty());

    CHECK(env.chdir(dir) == imza::Environment::ChdirResult::UNCHANGED);
}

TEST_CASE("workspace subscription fires on readiness")
{
    imza::Environment env;
    std::shared_ptr<const imza::WorkspaceEnvironment> captured;
    auto subscription = env.subscribe_to_workspace_change(
        [&] { captured = env.workspace(); });
    REQUIRE(wait_until_ready(env));
    CHECK(env.ready());
    CHECK(captured != nullptr);
    CHECK(captured == env.workspace());
}

TEST_CASE("workspace carries an instruction and project skills when rooted")
{
    const imza::test::CurrentDirectory directory;
    const imza::test::TempDir root_dir;
    const auto root = root_dir.path;
    const auto git  = root / ".git";
    REQUIRE(std::filesystem::create_directories(git));
    imza::test::write_file(root / "AGENTS.md", "agents rules");

    imza::Environment env;
    REQUIRE(wait_until_ready(env));
    REQUIRE(env.chdir(root) == imza::Environment::ChdirResult::CHANGED);
    const auto ws = env.workspace();
    REQUIRE(ws != nullptr);
    REQUIRE(ws->project_root.has_value());
    REQUIRE(ws->instruction.has_value());
    CHECK(ws->instruction->content == "agents rules");
    CHECK(env.agent_rules_path() == "AGENTS.md");
}

TEST_CASE("workspace scan retains nested cwd and discovers repository root")
{
    const imza::test::TempDir root_dir;
    const auto root = root_dir.path;
    std::error_code error;
    std::filesystem::create_directories(root / ".git", error);
    std::filesystem::create_directories(root / "nested" / "deeper", error);
    REQUIRE_FALSE(error);

    const auto workspace = imza::scan_workspace(root / "nested" / "deeper");
    CHECK(workspace.working_directory == root / "nested" / "deeper");
    CHECK(workspace.project_root == root);
}

TEST_CASE("load_agent_file selects the first available candidate")
{
    struct AgentFileCase {
        std::string_view name;
        std::vector<std::pair<std::string_view, std::string_view>> files;
        std::optional<imza::InstructionFile> expected;
    };
    const std::vector<AgentFileCase> cases {
        { "prefers AGENTS.md",
            { { "AGENTS.md", "agents rules" },
                { "CLAUDE.md", "claude rules" } },
            imza::InstructionFile { "AGENTS.md", "agents rules" } },
        { "falls back to GEMINI.md", { { "GEMINI.md", "gemini rules" } },
            imza::InstructionFile { "GEMINI.md", "gemini rules" } },
        { "returns nullopt without candidates", { }, std::nullopt },
    };

    for (const auto& test : cases) {
        CAPTURE(test.name);
        const imza::test::TempDir dir;
        for (const auto& [name, content] : test.files) {
            imza::test::write_file(dir.file(std::string(name)), content);
        }

        const auto found = imza::load_agent_file(dir.path);
        REQUIRE(found.has_value() == test.expected.has_value());
        if (test.expected) {
            CHECK(found->path == test.expected->path);
            CHECK(found->content == test.expected->content);
        }
    }
}

TEST_CASE("load_agent_file truncates oversized content")
{
    const imza::test::TempDir dir;
    imza::test::write_file(dir.file("AGENTS.md"), std::string(64 * 1024, 'x'));

    const auto found = imza::load_agent_file(dir.path);
    REQUIRE(found.has_value());
    CHECK(found->content.find("[truncated]") != std::string::npos);
    CHECK(found->content.size() < 64 * 1024);
}

TEST_CASE("load_instruction_file resolves relative paths against the root")
{
    const imza::test::TempDir dir;
    imza::test::write_file(dir.file("docs/guide.md"), "guide rules");

    const auto found = imza::load_instruction_file(dir.path, "docs/guide.md");
    REQUIRE(found.has_value());
    CHECK(found->path == (dir.path / "docs" / "guide.md").string());
    CHECK(found->content == "guide rules");
}

TEST_CASE("load_instruction_file skips missing files and honors absolutes")
{
    const imza::test::TempDir dir;
    CHECK_FALSE(imza::load_instruction_file(dir.path, "nope.md").has_value());

    imza::test::write_file(dir.file("outside.md"), "outside rules");
    const auto found = imza::load_instruction_file(
        dir.path / "elsewhere", dir.file("outside.md"));
    REQUIRE(found.has_value());
    CHECK(found->content == "outside rules");
}

TEST_CASE("load_instruction_file truncates oversized content")
{
    const imza::test::TempDir dir;
    imza::test::write_file(dir.file("big.md"), std::string(64 * 1024, 'x'));

    const auto found = imza::load_instruction_file(dir.path, "big.md");
    REQUIRE(found.has_value());
    CHECK(found->content.find("[truncated]") != std::string::npos);
    CHECK(found->content.size() < 64 * 1024);
}

TEST_CASE("workspace scan loads configured instructions in order")
{
    const imza::test::TempDir root_dir;
    const auto root = root_dir.path;
    std::error_code error;
    REQUIRE(std::filesystem::create_directories(root / ".git", error));
    imza::test::write_file(root / "AGENTS.md", "agents rules");
    imza::test::write_file(root / "docs" / "one.md", "one rules");
    imza::test::write_file(root / "two.md", "two rules");

    const auto workspace
        = imza::scan_workspace(root, { "docs/one.md", "two.md", "missing.md" });
    REQUIRE(workspace.instruction.has_value());
    CHECK(workspace.instruction->content == "agents rules");
    REQUIRE(workspace.extra_instructions.size() == 2);
    CHECK(workspace.extra_instructions[0].content == "one rules");
    CHECK(workspace.extra_instructions[1].content == "two rules");
}

TEST_CASE("configured instructions load outside a project root")
{
    const imza::test::TempDir root_dir;
    imza::test::write_file(root_dir.file("notes.md"), "loose notes");

    const auto workspace = imza::scan_workspace(root_dir.path, { "notes.md" });
    CHECK_FALSE(workspace.project_root.has_value());
    REQUIRE(workspace.extra_instructions.size() == 1);
    CHECK(workspace.extra_instructions[0].content == "loose notes");
}

} // namespace
