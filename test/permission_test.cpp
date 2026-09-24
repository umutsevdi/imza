#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "app/application_state.h"
#include "app/flows.h"
#include "conversation/persistence.h"
#include "network/json_io.h"
#include "permissions/evaluator.h"
#include "permissions/filesystem.h"
#include "permissions/shell.h"
#include "permissions/shell_analysis.h"
#include "permissions/store.h"
#include "platform/config.h"
#include "tools/skills.h"
#include "tools/tool.h"
#include "workspace/environment.h"

namespace imza {

namespace {

    class PermissionFixture {
    public:
        PermissionFixture()
        {
            const auto stamp
                = std::chrono::steady_clock::now().time_since_epoch().count();
            root = std::filesystem::temp_directory_path()
                / ("imza-permission-test-" + std::to_string(stamp));
            workspace = root / "workspace";
            temporary = root / "temporary";
            outside   = root / "outside";
            std::filesystem::create_directories(workspace);
            std::filesystem::create_directories(temporary);
            std::filesystem::create_directories(outside);
            write(workspace / "inside.txt");
            write(temporary / "temporary.txt");
            write(outside / "outside.txt");
            system                      = std::make_shared<SystemEnvironment>();
            system->temporary_directory = temporary;
            environment = std::make_shared<WorkspaceEnvironment>();
            environment->working_directory = workspace;
            environment->project_root      = workspace;
        }

        ~PermissionFixture()
        {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        PermissionContext context(
            Session::Mode mode, PermissionStore::Grants grants = { }) const
        {
            return { system, environment,
                std::make_shared<const PermissionStore::Grants>(
                    std::move(grants)),
                mode };
        }

        std::filesystem::path root;
        std::filesystem::path workspace;
        std::filesystem::path temporary;
        std::filesystem::path outside;
        std::shared_ptr<SystemEnvironment> system;
        std::shared_ptr<WorkspaceEnvironment> environment;

    private:
        static void write(const std::filesystem::path& path)
        {
            std::ofstream file(path);
            file << "content\n";
        }
    };

    void write_session_file(const std::filesystem::path& path,
        const std::filesystem::path& workspace, std::string_view title)
    {
        Json::Value root(Json::objectValue);
        root["workspace"] = workspace.string();
        root["title"]     = std::string(title);
        root["items"]     = Json::Value(Json::arrayValue);
        std::ofstream file(path);
        file << write_json(root);
    }

    FilesystemRequest read_request(const std::filesystem::path& path)
    {
        return ReadFileRequest { path };
    }

    FilesystemRequest list_request(const std::filesystem::path& path)
    {
        return ListDirectoryRequest { path };
    }

    FilesystemRequest find_request(
        const std::filesystem::path& path, std::string pattern = "content")
    {
        return FindFilesRequest { path, std::move(pattern) };
    }

    FilesystemRequest edit_request(const std::filesystem::path& path)
    {
        return EditFileRequest { path, "content", "updated", 1 };
    }

    FilesystemRequest write_request(const std::filesystem::path& path)
    {
        return WriteFileRequest { path, "updated" };
    }

    ShellEvaluation shell_policy(std::string command,
        const PermissionContext& context,
        std::chrono::seconds timeout = std::chrono::seconds(10))
    {
        return evaluate_shell_request(
            { std::move(command), timeout, { } }, context);
    }

} // namespace

TEST_CASE("permission store installs complete valid sets atomically")
{
    PermissionStore store;
    const auto empty_snapshot = store.snapshot();
    const std::filesystem::path root
        = std::filesystem::temp_directory_path().lexically_normal();
    const std::vector<PermissionGrant> valid {
        ExternalGrant { root },
        ShellCommandGrant { "git", "status" },
    };
    CHECK(store.install(valid));
    CHECK(empty_snapshot->empty());
    const auto installed_snapshot = store.snapshot();
    CHECK(installed_snapshot->size() == 2);
    CHECK(
        grants_cover(store.snapshot(), ShellCommandGrant { "git", "status" }));
    CHECK(store.install({ ExternalGrant { root } }));
    CHECK(store.snapshot()->size() == 2);
    CHECK(installed_snapshot->size() == 2);

    const std::vector<PermissionGrant> invalid {
        SkillGrant { root / "skill" },
        ExternalGrant { "relative" },
    };
    CHECK_FALSE(store.install(invalid));
    CHECK(store.snapshot()->size() == 2);
    store.clear();
    CHECK(store.snapshot()->empty());
}

TEST_CASE("permission store publishes only effective grant changes")
{
    PermissionStore store;
    int changes = 0;
    const auto subscription
        = store.subscribe_to_grants_change([&changes] { ++changes; });
    const PermissionGrant grant = ExternalGrant {
        std::filesystem::temp_directory_path().lexically_normal()
    };

    REQUIRE(store.install({ grant }));
    CHECK(changes == 1);
    REQUIRE(store.install({ grant }));
    CHECK(changes == 1);
    store.clear();
    CHECK(changes == 2);
    store.clear();
    CHECK(changes == 2);
}

TEST_CASE("runtime shell grants match subcommands and whole programs")
{
    PermissionStore store;
    const std::vector<PermissionGrant> grants { PermissionGrant {
        ShellCommandGrant { "git", "status" } } };
    REQUIRE(store.install(grants));
    CHECK(
        grants_cover(store.snapshot(), ShellCommandGrant { "git", "status" }));
    CHECK_FALSE(
        grants_cover(store.snapshot(), ShellCommandGrant { "git", "diff" }));
    REQUIRE(store.install({ ShellCommandGrant { "git", std::nullopt } }));
    CHECK(grants_cover(store.snapshot(), ShellCommandGrant { "git", "diff" }));
    CHECK(store.snapshot()->size() == 1);
}

TEST_CASE("shell analysis extracts compound command and subcommand pairs")
{
    const ShellAnalysis analysis
        = analyze_shell("git push origin && cmake --build build | tee log");
    REQUIRE(analysis.reuse == ShellAnalysis::Reuse::SESSION);
    REQUIRE(analysis.invocations.size() == 3);
    CHECK(analysis.invocations[0] == (ShellInvocation { "git", "push" }));
    CHECK(analysis.invocations[1] == (ShellInvocation { "cmake", "build" }));
    CHECK(analysis.invocations[2] == (ShellInvocation { "tee", "log" }));

    const ShellAnalysis flagged = analyze_shell("git -C build status");
    REQUIRE(flagged.invocations.size() == 1);
    CHECK(flagged.invocations[0] == (ShellInvocation { "git", "status" }));

    CHECK(analyze_shell("grep --color pattern src").invocations[0]
        == (ShellInvocation { "grep", "pattern" }));
    CHECK(analyze_shell("git status").invocations[0]
        == (ShellInvocation { "git", "status" }));
    CHECK(analyze_shell("git").invocations[0]
        == (ShellInvocation { "git", std::nullopt }));

    CHECK(
        analyze_shell("ls > listing.txt").reuse == ShellAnalysis::Reuse::ONCE);
    CHECK(analyze_shell("sh -c 'echo nested > listing.txt'").reuse
        == ShellAnalysis::Reuse::ONCE);
    CHECK(analyze_shell("echo $VALUE").reuse == ShellAnalysis::Reuse::ONCE);
    CHECK(analyze_shell("printf '%s\\n' literal").reuse
        == ShellAnalysis::Reuse::SESSION);
}

TEST_CASE("read-only command pair catalog is platform specific")
{
    const auto invocation = [](std::string_view command) {
        return analyze_shell(command).invocations.front();
    };

    CHECK(shell_readonly_allowed(invocation("git status")));
    CHECK(shell_readonly_allowed(invocation("git status -sb")));
    CHECK_FALSE(shell_readonly_allowed(invocation("git push")));
    CHECK_FALSE(shell_readonly_allowed(invocation("git")));
    CHECK_FALSE(shell_readonly_allowed(invocation("git status --force")));
    CHECK_FALSE(shell_readonly_allowed(invocation("git branch -D main")));
    CHECK(shell_readonly_allowed(invocation("git branch -a")));
    CHECK_FALSE(shell_readonly_allowed(invocation("git tag -d v1")));
    CHECK(shell_readonly_allowed(invocation("git tag -l")));
    CHECK_FALSE(shell_readonly_allowed(invocation("curl status")));
    CHECK_FALSE(shell_readonly_allowed(invocation("git diff --output=x.txt")));
    CHECK(shell_readonly_allowed(invocation("git diff --stat")));
    CHECK(shell_readonly_allowed(invocation("make -n")));
    CHECK(shell_readonly_allowed(invocation("which -a ls")));
#ifdef _WIN32
    CHECK(shell_readonly_allowed(invocation("tasklist /v")));
    CHECK_FALSE(shell_readonly_allowed(invocation("tasklist /kill")));
    CHECK_FALSE(shell_readonly_allowed(invocation("systemctl status")));
#else
    CHECK(shell_readonly_allowed(invocation("systemctl status")));
    CHECK_FALSE(shell_readonly_allowed(invocation("systemctl start foo")));
    CHECK_FALSE(shell_readonly_allowed(invocation("tasklist /v")));
#endif
}

TEST_CASE("application state shares grants with children")
{
    const auto immediate = [](std::function<void()> task) { task(); };
    auto parent          = make_application_state(immediate, Config { });
    const std::vector<PermissionGrant> grants { ExternalGrant {
        std::filesystem::current_path().lexically_normal() } };
    REQUIRE(parent->permissions->install(grants));
    auto child = make_child_application_state(*parent, immediate);
    CHECK(parent->permissions == child->permissions);
    CHECK(child->permissions->snapshot()->size() == 1);
    CHECK(parent->runtime_flags == child->runtime_flags);
    CHECK(parent->environment == child->environment);
    CHECK(parent->environment->system() == child->environment->system());
}

TEST_CASE("session lifecycle clears grants only after successful activation")
{
    PermissionFixture fixture;
    const auto immediate = [](std::function<void()> task) { task(); };
    auto state           = make_application_state(immediate, Config { });
    const auto grant     = ExternalGrant { fixture.outside };
    REQUIRE(state->permissions->install({ grant }));
    state->session->set_title("Current session");

    enqueue_user_modal(*state, SessionsModal { });
    resolve_modal(*state, fixture.root / "missing-session.json");
    CHECK(state->session->title() == "Current session");
    CHECK(state->permissions->snapshot()->size() == 1);

    const auto path = fixture.root / "session.json";
    const auto workspace
        = std::filesystem::weakly_canonical(std::filesystem::current_path());
    write_session_file(path, workspace, "Loaded session");
    enqueue_user_modal(*state, SessionsModal { });
    resolve_modal(*state, path);
    CHECK(state->session->title() == "Loaded session");
    CHECK(state->session->error().empty());
    CHECK(state->permissions->snapshot()->empty());

    REQUIRE(state->permissions->install({ grant }));
    CHECK(save_session(*state->session) == Status::OK);
    CHECK(state->permissions->snapshot()->size() == 1);
    run_slash(*state, "/new");
    CHECK(state->permissions->snapshot()->empty());
}

TEST_CASE("failed directory changes and child creation retain grants")
{
    PermissionFixture fixture;
    const auto immediate = [](std::function<void()> task) { task(); };
    auto parent          = make_application_state(immediate, Config { });
    const auto grant     = ExternalGrant { fixture.outside };
    REQUIRE(parent->permissions->install({ grant }));

    CHECK(parent->environment->chdir(fixture.root / "missing")
        == imza::Environment::ChdirResult::FAILED);
    CHECK(parent->permissions->snapshot()->size() == 1);
}

TEST_CASE("permission store snapshots remain valid during concurrent changes")
{
    PermissionFixture fixture;
    PermissionStore store;
    const std::filesystem::path root = fixture.outside;
    std::atomic<bool> start { false };
    std::atomic<bool> snapshots_valid { true };
    std::vector<std::jthread> readers;
    for (int index = 0; index < 4; ++index) {
        readers.emplace_back([&] {
            while (!start.load()) {
                std::this_thread::yield();
            }
            for (int iteration = 0; iteration < 500; ++iteration) {
                const auto snapshot = store.snapshot();
                for (const PermissionGrant& grant : *snapshot) {
                    const auto* path = std::get_if<ExternalGrant>(&grant);
                    if (path == nullptr || !path->is_absolute()) {
                        snapshots_valid.store(false);
                    }
                }
            }
        });
    }
    start.store(true);
    for (int iteration = 0; iteration < 100; ++iteration) {
        const auto directory
            = root / ("permission-" + std::to_string(iteration));
        std::filesystem::create_directory(directory);
        REQUIRE(store.install({ ExternalGrant { directory } }));
        if (iteration % 7 == 0) {
            store.clear();
        }
    }
    readers.clear();
    CHECK(snapshots_valid.load());
    store.clear();
    CHECK(store.snapshot()->empty());
}

TEST_CASE("filesystem policy covers all trusted and granted write cases")
{
    PermissionFixture fixture;

    const auto planned = evaluate_filesystem_request(
        write_request(fixture.workspace / "new.txt"),
        fixture.context(Session::Mode::PLAN));
    CHECK(planned.decision.kind == PermissionDecision::Kind::REJECT);
    CHECK_FALSE(planned.request.has_value());

    const auto built = evaluate_filesystem_request(
        write_request(fixture.workspace / "new.txt"),
        fixture.context(Session::Mode::BUILD));
    CHECK(built.decision.kind == PermissionDecision::Kind::ACCEPT);

    const auto workspace_edit = evaluate_filesystem_request(
        edit_request(fixture.workspace / "inside.txt"),
        fixture.context(Session::Mode::PLAN));
    CHECK(workspace_edit.decision.kind == PermissionDecision::Kind::REJECT);

    const auto temporary_write = evaluate_filesystem_request(
        write_request(fixture.temporary / "new.txt"),
        fixture.context(Session::Mode::BUILD));
    CHECK(temporary_write.decision.kind == PermissionDecision::Kind::ACCEPT);

    const auto outside_write = evaluate_filesystem_request(
        write_request(fixture.outside / "new.txt"),
        fixture.context(Session::Mode::BUILD));
    CHECK(outside_write.decision.kind == PermissionDecision::Kind::ASK);

    const PermissionGrant grant = ExternalGrant { fixture.outside };
    const auto granted          = evaluate_filesystem_request(
        write_request(fixture.outside / "new.txt"),
        fixture.context(Session::Mode::BUILD, { grant }));
    CHECK(granted.decision.kind == PermissionDecision::Kind::ACCEPT);
}

TEST_CASE("filesystem policy trusts only configured roots")
{
    PermissionFixture fixture;
    REQUIRE(fixture.environment->working_directory == fixture.workspace);
    REQUIRE(fixture.environment->project_root == fixture.workspace);
    REQUIRE(fixture.system->temporary_directory == fixture.temporary);
    const auto workspace_read = evaluate_filesystem_request(
        read_request(fixture.workspace / "inside.txt"),
        fixture.context(Session::Mode::PLAN));
    CHECK(workspace_read.decision.kind == PermissionDecision::Kind::ACCEPT);

    const auto temporary_read = evaluate_filesystem_request(
        read_request(fixture.temporary / "temporary.txt"),
        fixture.context(Session::Mode::PLAN));
    CHECK(temporary_read.decision.kind == PermissionDecision::Kind::ACCEPT);

    const auto outside_read = evaluate_filesystem_request(
        read_request(fixture.outside / "outside.txt"),
        fixture.context(Session::Mode::PLAN));
    CHECK(outside_read.decision.kind == PermissionDecision::Kind::ASK);

    const auto outside_list = evaluate_filesystem_request(
        list_request(fixture.outside), fixture.context(Session::Mode::PLAN));
    CHECK(outside_list.decision.kind == PermissionDecision::Kind::ACCEPT);

    const auto outside_find = evaluate_filesystem_request(
        find_request(fixture.outside), fixture.context(Session::Mode::PLAN));
    CHECK(outside_find.decision.kind == PermissionDecision::Kind::ACCEPT);
    REQUIRE(outside_find.request.has_value());
    CHECK(filesystem_target(*outside_find.request) == fixture.outside);
}

TEST_CASE("filesystem policy falls back to the working directory")
{
    PermissionFixture fixture;
    fixture.environment->project_root.reset();

    const auto inside = evaluate_filesystem_request(
        read_request(fixture.workspace / "inside.txt"),
        fixture.context(Session::Mode::PLAN));
    const auto outside = evaluate_filesystem_request(
        read_request(fixture.outside / "outside.txt"),
        fixture.context(Session::Mode::PLAN));

    CHECK(inside.decision.kind == PermissionDecision::Kind::ACCEPT);
    CHECK(outside.decision.kind == PermissionDecision::Kind::ASK);
}

TEST_CASE("filesystem policy trusts the repository above a nested cwd")
{
    PermissionFixture fixture;
    const auto nested = fixture.workspace / "nested";
    std::filesystem::create_directories(nested);
    fixture.environment->working_directory = nested;

    const auto result = evaluate_filesystem_request(
        read_request("../inside.txt"), fixture.context(Session::Mode::PLAN));
    CHECK(result.decision.kind == PermissionDecision::Kind::ACCEPT);
    REQUIRE(result.request.has_value());
    CHECK(
        filesystem_target(*result.request) == fixture.workspace / "inside.txt");
}

TEST_CASE("external directory grants cover descendants but not siblings")
{
    PermissionFixture fixture;
    const auto target               = fixture.outside / "outside.txt";
    const PermissionGrant directory = ExternalGrant { fixture.outside };
    const auto recursive = evaluate_filesystem_request(read_request(target),
        fixture.context(Session::Mode::PLAN, { directory }));
    CHECK(recursive.decision.kind == PermissionDecision::Kind::ACCEPT);

    const auto sibling = fixture.root / "outside-sibling";
    std::filesystem::create_directories(sibling);
    std::ofstream(sibling / "file.txt") << "content\n";
    const auto sibling_result
        = evaluate_filesystem_request(read_request(sibling / "file.txt"),
            fixture.context(Session::Mode::PLAN, { directory }));
    CHECK(sibling_result.decision.kind == PermissionDecision::Kind::ASK);
}

TEST_CASE("one external grant authorizes mode-available operations")
{
    PermissionFixture fixture;
    const auto target = fixture.outside / "outside.txt";
    const auto read   = evaluate_filesystem_request(
        read_request(target), fixture.context(Session::Mode::PLAN));
    REQUIRE(read.decision.kind == PermissionDecision::Kind::ASK);
    REQUIRE(read.request.has_value());
    const auto grant = filesystem_session_grant(*read.request);
    REQUIRE(grant.has_value());
    CHECK(*grant == fixture.outside);

    const auto edit = evaluate_filesystem_request(edit_request(target),
        fixture.context(Session::Mode::BUILD, { PermissionGrant { *grant } }));
    CHECK(edit.decision.kind == PermissionDecision::Kind::ACCEPT);
}

TEST_CASE("filesystem normalization rejects malformed and wrong-type targets")
{
    PermissionFixture fixture;
    const auto empty_path
        = evaluate_filesystem_request(read_request(std::filesystem::path { }),
            fixture.context(Session::Mode::PLAN));
    CHECK(empty_path.decision.kind == PermissionDecision::Kind::REJECT);

    const auto wrong_type = evaluate_filesystem_request(
        read_request(fixture.workspace), fixture.context(Session::Mode::PLAN));
    CHECK(wrong_type.decision.kind == PermissionDecision::Kind::REJECT);

    const auto relative = evaluate_filesystem_request(
        read_request("inside.txt"), fixture.context(Session::Mode::PLAN));
    CHECK(relative.decision.kind == PermissionDecision::Kind::ACCEPT);
    REQUIRE(relative.request.has_value());
    CHECK(filesystem_target(*relative.request)
        == fixture.workspace / "inside.txt");
}

TEST_CASE("filesystem normalization resolves traversal and missing writes")
{
    PermissionFixture fixture;
    std::filesystem::create_directories(fixture.workspace / "nested");

    const auto traversal
        = evaluate_filesystem_request(read_request("nested/../inside.txt"),
            fixture.context(Session::Mode::PLAN));
    CHECK(traversal.decision.kind == PermissionDecision::Kind::ACCEPT);
    REQUIRE(traversal.request.has_value());
    CHECK(filesystem_target(*traversal.request)
        == fixture.workspace / "inside.txt");

    const auto missing = evaluate_filesystem_request(
        write_request(fixture.workspace / "nested" / "new.txt"),
        fixture.context(Session::Mode::BUILD));
    CHECK(missing.decision.kind == PermissionDecision::Kind::ACCEPT);
    REQUIRE(missing.request.has_value());
    CHECK(filesystem_target(*missing.request)
        == fixture.workspace / "nested" / "new.txt");

    const auto missing_parent = evaluate_filesystem_request(
        write_request(fixture.workspace / "absent" / "new.txt"),
        fixture.context(Session::Mode::BUILD));
    CHECK(missing_parent.decision.kind == PermissionDecision::Kind::REJECT);
}

TEST_CASE("filesystem normalization follows symlinked targets and parents")
{
#ifdef _WIN32
    return;
#else
    PermissionFixture fixture;
    std::filesystem::create_directory_symlink(
        fixture.outside, fixture.workspace / "outside-link");
    const auto linked_read = evaluate_filesystem_request(
        read_request(fixture.workspace / "outside-link" / "outside.txt"),
        fixture.context(Session::Mode::PLAN));
    CHECK(linked_read.decision.kind == PermissionDecision::Kind::ASK);
    REQUIRE(linked_read.request.has_value());
    CHECK(filesystem_target(*linked_read.request)
        == fixture.outside / "outside.txt");

    const auto linked_write = evaluate_filesystem_request(
        write_request(fixture.workspace / "outside-link" / "new.txt"),
        fixture.context(Session::Mode::BUILD));
    CHECK(linked_write.decision.kind == PermissionDecision::Kind::ASK);
    REQUIRE(linked_write.request.has_value());
    CHECK(filesystem_target(*linked_write.request)
        == fixture.outside / "new.txt");
#endif
}

TEST_CASE("central evaluator assigns explicit policies to built-in tools")
{
    PermissionFixture fixture;
    const PermissionContext plan = fixture.context(Session::Mode::PLAN);
    const Config config;
    const std::vector<Skill> skills;
    SkillStore loaded;

    const auto evaluate = [&](std::string name, std::string arguments) {
        return evaluate_tool_request(
            { std::move(name), std::move(arguments), "", "" }, plan, config,
            skills, loaded);
    };

    CHECK(evaluate("subagent",
              R"({"tasks":[{"mode":"research","prompt":"inspect"}]})")
              .decision.kind
        == PermissionDecision::Kind::ACCEPT);
    CHECK(evaluate(
              "subagent", R"({"tasks":[{"mode":"build","prompt":"change"}]})")
              .decision.kind
        == PermissionDecision::Kind::REJECT);
    CHECK(evaluate("write", R"({"file_path":"new.txt"})").decision.kind
        == PermissionDecision::Kind::REJECT);
    CHECK(evaluate("custom", "{}").decision.kind
        == PermissionDecision::Kind::REJECT);
    CHECK(evaluate("lua", R"js({"script":"print(1)"})js").decision.kind
        == PermissionDecision::Kind::ACCEPT);
    CHECK(evaluate("lua", R"js({"script":""})js").decision.kind
        == PermissionDecision::Kind::REJECT);
}

TEST_CASE("shell policy reuses, combines, and broadens session grants")
{
    PermissionFixture fixture;
    const Config config;
    const std::vector<Skill> skills;
    SkillStore loaded;
    const auto evaluate
        = [&](std::string command, PermissionStore::Grants grants = { }) {
              return shell_policy(std::move(command),
                  fixture.context(Session::Mode::BUILD, std::move(grants)));
          };

#ifdef _WIN32
    CHECK(evaluate("dir /b").decision.kind == PermissionDecision::Kind::ACCEPT);
    CHECK_FALSE(shell_builtin_allowed("ls"));
#else
    CHECK(evaluate("ls -la").decision.kind == PermissionDecision::Kind::ACCEPT);
    CHECK_FALSE(shell_builtin_allowed("dir"));
#endif

    const ShellEvaluation first = evaluate("git push origin");
    REQUIRE(first.decision.kind == PermissionDecision::Kind::ASK);
    REQUIRE(first.session_grants.size() == 1);
    CHECK(std::get<ShellCommandGrant>(first.session_grants.front())
        == (ShellCommandGrant { "git", "push" }));
    CHECK(evaluate("git push --force-with-lease", first.session_grants)
              .decision.kind
        == PermissionDecision::Kind::ACCEPT);

    const ShellEvaluation broader
        = evaluate("git add file.cpp", first.session_grants);
    REQUIRE(broader.decision.kind == PermissionDecision::Kind::ASK);
    REQUIRE(broader.session_grants.size() == 1);
    CHECK(std::get<ShellCommandGrant>(broader.session_grants.front())
        == (ShellCommandGrant { "git", std::nullopt }));
    CHECK(broader.decision.reason.find("git *") != std::string::npos);

    const ShellEvaluation compound = evaluate("cargo test && ninja -C build");
    REQUIRE(compound.decision.kind == PermissionDecision::Kind::ASK);
    CHECK(compound.session_grants.size() == 2);

    CHECK(evaluate("git status && git log").decision.kind
        == PermissionDecision::Kind::ACCEPT);
    CHECK(evaluate("git status && git push").decision.kind
        == PermissionDecision::Kind::ASK);
    const ShellAnalysis mixed = analyze_shell("git status && git push");
    CHECK_FALSE(shell_readonly_allowed(mixed.invocations[1]));

    CHECK(evaluate("make -n && which -a ls").decision.kind
        == PermissionDecision::Kind::ACCEPT);
    CHECK(evaluate("git branch -D main").decision.kind
        == PermissionDecision::Kind::ASK);
    CHECK(evaluate("git diff --output=patch.txt").decision.kind
        == PermissionDecision::Kind::ASK);

    const ShellEvaluation redirected = evaluate("ls > files.txt");
    CHECK(redirected.decision.kind == PermissionDecision::Kind::ASK);
    CHECK(redirected.session_grants.empty());
    CHECK(redirected.request.command == "ls > files.txt");
}

TEST_CASE("shell policy rejects an empty command")
{
    PermissionFixture fixture;
    const auto empty = shell_policy("", fixture.context(Session::Mode::BUILD));
    CHECK(empty.decision.kind == PermissionDecision::Kind::REJECT);
}

TEST_CASE("skill policy and runtime grants use the same central evaluator")
{
    PermissionFixture fixture;
    const auto path = fixture.outside / "SKILL.md";
    {
        std::ofstream file(path);
        file << "Use the documented workflow.";
    }
    const Skill skill { "docs", "Documentation workflow", path,
        Skill::Scope::GLOBAL, std::nullopt };
    const std::vector<Skill> skills { skill };
    SkillStore loaded;
    Config config;
    const ToolCallRequest request { "skill", R"({"name":"docs"})", "", "" };

    auto evaluation = evaluate_tool_request(
        request, fixture.context(Session::Mode::PLAN), config, skills, loaded);
    CHECK(evaluation.decision.kind == PermissionDecision::Kind::ASK);
    REQUIRE(evaluation.prompt.has_value());
    CHECK(evaluation.prompt->allow_for_session);
    CHECK(evaluation.prompt->reason == "skill instructions require approval");
    CHECK(std::get<imza::SkillRequest>(evaluation.prompt->request).name
        == "docs");
    REQUIRE(evaluation.session_grants.size() == 1);

    evaluation = evaluate_tool_request(request,
        fixture.context(Session::Mode::PLAN, evaluation.session_grants), config,
        skills, loaded);
    CHECK(evaluation.decision.kind == PermissionDecision::Kind::ACCEPT);

    config.global_skills["docs"] = SkillPolicy::ALLOW;
    CHECK(evaluate_tool_request(request, fixture.context(Session::Mode::PLAN),
              config, skills, loaded)
              .decision.kind
        == PermissionDecision::Kind::ACCEPT);
    config.global_skills["docs"] = SkillPolicy::DENY;
    CHECK(evaluate_tool_request(request, fixture.context(Session::Mode::PLAN),
              config, skills, loaded)
              .decision.kind
        == PermissionDecision::Kind::REJECT);

    config.global_skills["docs"] = SkillPolicy::ASK;
    std::string error;
    REQUIRE(loaded.load(skill, error));
    CHECK(evaluate_tool_request(request, fixture.context(Session::Mode::PLAN),
              config, skills, loaded)
              .decision.kind
        == PermissionDecision::Kind::ACCEPT);

    const Skill missing { "missing", "Missing instructions",
        fixture.outside / "missing.md", Skill::Scope::GLOBAL, std::nullopt };
    CHECK(evaluate_tool_request({ "skill", R"({"name":"missing"})", "", "" },
              fixture.context(Session::Mode::PLAN), config,
              std::vector<Skill> { missing }, loaded)
              .decision.kind
        == PermissionDecision::Kind::REJECT);

    const auto large_path = fixture.outside / "large.md";
    {
        std::ofstream file(large_path);
        file << std::string(128 * 1024 + 1, 'x');
    }
    const Skill large { "large", "Large instructions", large_path,
        Skill::Scope::GLOBAL, std::nullopt };
    CHECK(evaluate_tool_request({ "skill", R"({"name":"large"})", "", "" },
              fixture.context(Session::Mode::PLAN), config,
              std::vector<Skill> { large }, loaded)
              .decision.kind
        == PermissionDecision::Kind::REJECT);
}

TEST_CASE("authorized_skill_path accepts only the canonical target")
{
    PermissionFixture fixture;
    const auto path = fixture.outside / "SKILL.md";
    {
        std::ofstream file(path);
        file << "workflow";
    }
    const Skill skill { "docs", "Documentation workflow", path,
        Skill::Scope::GLOBAL, std::nullopt };
    const std::string canonical = canonical_skill_path(skill)->string();

    Json::Value bound(Json::objectValue);
    bound["name"]  = "docs";
    bound["scope"] = "global";
    bound["path"]  = canonical;
    CHECK(authorized_skill_path(skill, { "skill", write_json(bound), "", "" })
        == canonical_skill_path(skill));

    // A request aimed at a different file must not authorize this skill.
    Json::Value other(Json::objectValue);
    other["name"]  = "docs";
    other["scope"] = "global";
    other["path"]  = (fixture.outside / "elsewhere.md").string();
    CHECK_FALSE(
        authorized_skill_path(skill, { "skill", write_json(other), "", "" }));

    // Nor one that omits the path, as an unnormalized model call does.
    CHECK_FALSE(authorized_skill_path(
        skill, { "skill", R"({"name":"docs"})", "", "" }));
}

TEST_CASE("skill evaluation binds approval to the canonical instruction path")
{
#ifdef _WIN32
    return;
#else
    PermissionFixture fixture;
    const auto first  = fixture.outside / "first-skill.md";
    const auto second = fixture.outside / "second-skill.md";
    const auto link   = fixture.outside / "active-skill.md";
    {
        std::ofstream file(first);
        file << "first";
    }
    {
        std::ofstream file(second);
        file << "second";
    }
    std::filesystem::create_symlink(first, link);

    const Skill skill { "changing", "Changing skill", link,
        Skill::Scope::GLOBAL, std::nullopt };
    const std::vector<Skill> skills { skill };
    const Config config;
    SkillStore loaded;
    const ToolCallRequest request { "skill", R"({"name":"changing"})", "", "" };
    const PermissionEvaluation approved = evaluate_tool_request(
        request, fixture.context(Session::Mode::PLAN), config, skills, loaded);
    REQUIRE(approved.decision.kind == PermissionDecision::Kind::ASK);
    CHECK(
        parse_json(approved.request.args)["path"].asString() == first.string());

    std::filesystem::remove(link);
    std::filesystem::create_symlink(second, link);
    const PermissionEvaluation current = evaluate_tool_request(approved.request,
        fixture.context(Session::Mode::PLAN), config, skills, loaded);

    REQUIRE(current.decision.kind == PermissionDecision::Kind::ASK);
    CHECK(current.request.args != approved.request.args);
    CHECK(
        parse_json(current.request.args)["path"].asString() == second.string());
#endif
}

} // namespace imza
