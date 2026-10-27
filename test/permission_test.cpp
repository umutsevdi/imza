#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "app/application_state.h"
#include "app/flows.h"
#include "conversation/persistence.h"
#include "loopback_mcp_server.h"
#include "network/json.h"
#include "network/json_io.h"
#include "permissions/evaluator.h"
#include "permissions/filesystem.h"
#include "permissions/permission_store.h"
#include "permissions/shell.h"
#include "permissions/shell_analysis.h"
#include "platform/config.h"
#include "runtime/mcp_subsystem.h"
#include "test_fs.h"
#include "test_helpers.h"
#include "test_state.h"
#include "tools/skills.h"
#include "tools/tool.h"
#include "workspace/environment.h"

namespace imza {

namespace {

    imza::McpServerConfig server_config(
        const std::string& id, const std::string& url)
    {
        imza::McpServerConfig config;
        config.id  = id;
        config.url = url;
        return config;
    }

    bool wait_state(imza::McpSubsystem& manager, const std::string& id,
        imza::McpServerState want)
    {
        for (int i = 0; i < 1000; ++i) {
            for (const auto& server : manager.snapshot()) {
                if (server.id == id && server.state == want) {
                    return true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    class PermissionFixture {
    public:
        PermissionFixture()
        {
            workspace = _dir.file("workspace");
            temporary = _dir.file("temporary");
            outside   = _dir.file("outside");
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

        std::filesystem::path root() const { return _dir.path; }

        PermissionContext context(
            Session::Mode mode, PermissionStore::Grants grants = { }) const
        {
            return { system, environment,
                std::make_shared<const PermissionStore::Grants>(
                    std::move(grants)),
                mode };
        }

        PermissionContext context(Session::Mode mode,
            PermissionStore::Grants grants,
            std::vector<std::filesystem::path> skill_directories,
            const McpSubsystem* mcp = nullptr) const
        {
            return { system, environment,
                std::make_shared<const PermissionStore::Grants>(
                    std::move(grants)),
                mode, std::move(skill_directories), mcp };
        }

        std::filesystem::path workspace;
        std::filesystem::path temporary;
        std::filesystem::path outside;
        std::shared_ptr<SystemEnvironment> system;
        std::shared_ptr<WorkspaceEnvironment> environment;

    private:
        imza::test::TempDir _dir;

        static void write(const std::filesystem::path& path)
        {
            imza::test::write_file(path, "content\n");
        }
    };

    void write_session_file(const std::filesystem::path& path,
        const std::filesystem::path& workspace, std::string_view title)
    {
        imza::JsonValue root = JsonValue::object_t { };
        auto& object         = root.get<JsonValue::object_t>();
        object["workspace"]  = JsonValue(workspace.string());
        object["title"]      = JsonValue(std::string(title));
        object["items"]      = JsonValue(JsonValue::array_t { });
        std::ofstream file(path);
        file << imza::json_dump(root);
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

    FilesystemRequest mkdir_request(const std::filesystem::path& path)
    {
        return MakeDirectoryRequest { path };
    }

    FilesystemRequest remove_request(const std::filesystem::path& path)
    {
        return RemoveRequest { path };
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

TEST_CASE("permission store exposes the live approval-skipping switch")
{
    PermissionStore store;
    CHECK_FALSE(store.skip_permissions());

    bool notified = false;
    const auto subscription
        = store.subscribe_to_skip_change([&] { notified = true; });

    store.set_skip_permissions(true);
    CHECK(store.skip_permissions());
    CHECK(notified);

    notified = false;
    store.set_skip_permissions(true);
    CHECK_FALSE(notified);

    store.set_skip_permissions(false);
    CHECK_FALSE(store.skip_permissions());
    CHECK(notified);
}

TEST_CASE("approval skipping is shared with children and the sidechat")
{
    auto parent = imza::test::make_test_state();
    parent->permissions->set_skip_permissions(true);
    auto child = imza::make_child_application_state(
        *parent, imza::test::run_immediately);
    auto sidechat = imza::make_sidechat_application_state(*parent);
    CHECK(parent->permissions == child->permissions);
    CHECK(parent->permissions == sidechat->permissions);
    CHECK(child->permissions->skip_permissions());
    sidechat->permissions->set_skip_permissions(false);
    CHECK_FALSE(parent->permissions->skip_permissions());
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
    const auto first = [](std::string_view command) {
        return analyze_shell(command).invocations.front();
    };

    const ShellAnalysis analysis
        = analyze_shell("git push origin && cmake --build build | tee log");
    REQUIRE(analysis.reuse == ShellAnalysis::Reuse::SESSION);
    REQUIRE(analysis.invocations.size() == 3);
    CHECK(analysis.invocations[0].program == "git");
    CHECK(analysis.invocations[0].subcommand == "push");
    CHECK(analysis.invocations[0].arguments
        == std::vector<std::string> { "origin" });
    CHECK(analysis.invocations[1].program == "cmake");
    CHECK(analysis.invocations[1].subcommand == "build");
    CHECK(analysis.invocations[2].program == "tee");
    CHECK(analysis.invocations[2].subcommand == "log");

    const ShellAnalysis flagged = analyze_shell("git -C build status");
    REQUIRE(flagged.invocations.size() == 1);
    CHECK(flagged.invocations[0].program == "git");
    CHECK(flagged.invocations[0].subcommand == "status");
    CHECK(flagged.invocations[0].options == std::vector<std::string> { "-C" });

    CHECK(first("grep --color pattern src").program == "grep");
    CHECK(first("grep --color pattern src").subcommand == "pattern");
    CHECK(first("git status").subcommand == "status");
    CHECK_FALSE(first("git").subcommand.has_value());

    CHECK(
        analyze_shell("ls > listing.txt").reuse == ShellAnalysis::Reuse::ONCE);
    CHECK(analyze_shell("sh -c 'echo nested > listing.txt'").reuse
        == ShellAnalysis::Reuse::ONCE);
    CHECK(analyze_shell("echo $VALUE").reuse == ShellAnalysis::Reuse::ONCE);
    CHECK(analyze_shell("printf '%s\\n' literal").reuse
        == ShellAnalysis::Reuse::SESSION);
}

TEST_CASE("only a redirect that writes a real file costs the session grant")
{
    const auto reuse
        = [](std::string_view command) { return analyze_shell(command).reuse; };

    // Discarding output or duplicating a stream writes nothing.
    CHECK(reuse("git status 2>/dev/null") == ShellAnalysis::Reuse::SESSION);
    CHECK(reuse("git status > /dev/null") == ShellAnalysis::Reuse::SESSION);
    CHECK(reuse("git status >/dev/null 2>&1") == ShellAnalysis::Reuse::SESSION);
    CHECK(reuse("git status 2>&1") == ShellAnalysis::Reuse::SESSION);
    CHECK(reuse("git status 1>&2") == ShellAnalysis::Reuse::SESSION);
    CHECK(reuse("git status >&2") == ShellAnalysis::Reuse::SESSION);
    CHECK(reuse("cat x < y") == ShellAnalysis::Reuse::SESSION);

    // Writing a file, appending, or a heredoc does.
    CHECK(reuse("git status > out.txt") == ShellAnalysis::Reuse::ONCE);
    CHECK(reuse("git status >> out.txt") == ShellAnalysis::Reuse::ONCE);
    CHECK(reuse("git status &> out.txt") == ShellAnalysis::Reuse::ONCE);
    CHECK(reuse("git status 2> err.txt") == ShellAnalysis::Reuse::ONCE);
    CHECK(reuse("git status 2>/dev/null > out.txt")
        == ShellAnalysis::Reuse::ONCE);
    CHECK(reuse("cat <<EOF") == ShellAnalysis::Reuse::ONCE);

    // The target is never mistaken for an argument of the command.
    const ShellAnalysis redirected = analyze_shell("git status > out.txt");
    REQUIRE(redirected.invocations.size() == 1);
    CHECK(redirected.invocations[0].arguments.empty());
}

TEST_CASE("launcher unwrapping flattens code into classifiable invocations")
{
    const auto invocation = [](std::string_view command) {
        return analyze_shell(command).invocations.front();
    };

    const ShellAnalysis single = analyze_shell("bash -c 'ls'");
    REQUIRE(single.invocations.size() == 1);
    CHECK(single.invocations[0].program == "ls");
    CHECK_FALSE(single.invocations[0].unresolvable);

    const ShellAnalysis piped = analyze_shell("bash -c 'ls | grep x'");
    CHECK(piped.invocations.size() == 2);

    const ShellAnalysis deleted = analyze_shell("bash -c 'rm -rf build'");
    REQUIRE(deleted.invocations.size() == 1);
    CHECK(deleted.invocations[0].program == "rm");

    CHECK(invocation("env FOO=bar ls").program == "ls");
    CHECK(invocation("sudo ls").program == "ls");
    CHECK(invocation("sudo -u root ls").program == "ls");
    CHECK(invocation("env FOO=bar rm -rf x").program == "rm");

    CHECK(invocation("cmd /c \"dir\"").program == "dir");
#ifdef _WIN32
    // Program names are folded to lower case on Windows.
    CHECK(invocation("powershell -c \"Get-ChildItem\"").program
        == "get-childitem");
#else
    CHECK(invocation("powershell -c \"Get-ChildItem\"").program
        == "Get-ChildItem");
#endif

    CHECK(invocation("bash script.sh").unresolvable);
    CHECK(invocation("python3 -c 'print(1)'").unresolvable);
    CHECK(invocation("awk 'BEGIN{system(\"ls\")}'").unresolvable);
    CHECK(invocation("bash").unresolvable);
}

TEST_CASE("launcher programs are never grantable")
{
    for (std::string_view program : { "bash", "sh", "zsh", "dash", "cmd",
             "cmd.exe", "powershell", "pwsh", "env", "sudo", "xargs", "nice",
             "nohup", "time", "stdbuf", "command", "exec", "python", "python3",
             "perl", "ruby", "node", "php", "lua", "awk", "gawk" }) {
        CHECK(shell_launcher_denied(program));
    }
    CHECK_FALSE(shell_launcher_denied("ls"));
    CHECK_FALSE(shell_launcher_denied("git"));
    CHECK_FALSE(shell_launcher_denied("cmake"));

    PermissionStore store;
    CHECK_FALSE(store.install({ ShellCommandGrant { "bash", std::nullopt } }));
    CHECK_FALSE(store.install({ ShellCommandGrant { "env", "ls" } }));
    CHECK(store.snapshot()->empty());
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
    CHECK(shell_readonly_allowed(invocation("git status --porcelain=v1")));
    CHECK(shell_readonly_allowed(invocation("git log --since=2026-10-15")));
    CHECK(shell_readonly_allowed(invocation("git log --oneline -5")));
    CHECK(shell_readonly_allowed(invocation("git log -1 --format=%h %s")));
    CHECK(shell_readonly_allowed(invocation("git grep -n x -- src")));
    CHECK(
        shell_readonly_allowed(invocation("git rev-parse --abbrev-ref HEAD")));
    CHECK(shell_readonly_allowed(invocation("git describe --always --dirty")));
    CHECK_FALSE(shell_readonly_allowed(invocation("make -n")));
    CHECK_FALSE(shell_readonly_allowed(invocation("whois example.com")));
    CHECK_FALSE(shell_readonly_allowed(invocation("terraform plan")));
#ifdef _WIN32
    CHECK(shell_builtin_allowed("where"));
    CHECK(shell_builtin_allowed("netstat"));
    CHECK_FALSE(shell_readonly_allowed(invocation("systemctl status")));
#else
    CHECK(shell_readonly_allowed(invocation("systemctl status")));
    CHECK_FALSE(shell_readonly_allowed(invocation("systemctl start foo")));
    CHECK_FALSE(shell_readonly_allowed(invocation("tasklist /v")));
#endif
}

TEST_CASE("a catalog row decides which operands and options stay read-only")
{
    const auto allowed = [](std::string_view command) {
        return shell_readonly_allowed(
            analyze_shell(command).invocations.front());
    };

    // The same slot is a write for the sibling form.
    CHECK_FALSE(allowed("git remote add origin u"));
    CHECK_FALSE(allowed("git remote remove o"));
    CHECK_FALSE(allowed("git branch newbranch"));
    CHECK_FALSE(allowed("git branch -v newbranch"));
    CHECK_FALSE(allowed("git tag v1"));
    CHECK_FALSE(allowed("git tag -n v1"));
    CHECK_FALSE(allowed("git config user.name Bob"));
    CHECK_FALSE(allowed("git stash"));
    CHECK_FALSE(allowed("git stash drop"));
    CHECK_FALSE(allowed("git worktree add ../x"));
    CHECK_FALSE(allowed("git reflog expire --all"));

    CHECK(allowed("git config --get user.name"));
    CHECK(allowed("git branch --contains v1"));
    CHECK(allowed("git stash list"));
    CHECK(allowed("git worktree list"));
    CHECK(allowed("git reflog"));

    CHECK(allowed("git -C build status"));
    CHECK(allowed("git --no-pager log"));
    CHECK_FALSE(allowed("git -c core.pager=x log"));
    CHECK_FALSE(allowed("git --git-dir=/tmp/x log"));
    CHECK_FALSE(allowed("git --exec-path=/tmp/x log"));
}

TEST_CASE("read-only subcommand catalog rows match their intended commands")
{
    const auto invocation = [](std::string_view command) {
        return analyze_shell(command).invocations.front();
    };
#ifndef _WIN32
    for (std::string_view command :
        { "systemctl is-active", "systemctl is-enabled", "systemctl is-failed",
            "systemctl show", "systemctl cat", "systemctl list-dependencies",
            "systemctl get-default", "dnf repolist", "dnf search foo",
            "dnf provides foo", "dnf check-update", "cargo metadata",
            "cargo locate-project", "gem which rake", "gem environment",
            "gem contents rake" }) {
        CHECK(shell_readonly_allowed(invocation(command)));
    }
#endif
    for (std::string_view command : { "docker version", "docker info",
             "docker top", "docker port", "docker stats", "docker history",
             "docker diff", "docker search redis", "kubectl api-resources",
             "kubectl api-versions", "kubectl cluster-info",
             "kubectl explain pod", "kubectl version", "npm why left-pad",
             "npm root", "npm prefix", "pip freeze", "pip check", "go env",
             "go list", "go doc fmt" }) {
        CHECK(shell_readonly_allowed(invocation(command)));
    }
    CHECK_FALSE(shell_readonly_allowed(invocation("docker run alpine")));
    CHECK_FALSE(shell_readonly_allowed(invocation("kubectl apply -f x.yaml")));
    CHECK_FALSE(shell_readonly_allowed(invocation("pip install requests")));
    CHECK_FALSE(shell_readonly_allowed(invocation("go build ./...")));
    CHECK_FALSE(shell_readonly_allowed(invocation("npm install")));
}

TEST_CASE("whole-program read-only commands are safe with any operand")
{
#ifdef _WIN32
    // The Windows catalogue is the cmd.exe builtin set, which shares almost
    // nothing with the POSIX one.
    for (std::string_view program : { "whoami", "hostname", "netstat", "type",
             "where", "tree", "dir", "tasklist", "systeminfo" }) {
        CHECK(shell_builtin_allowed(program));
    }
    for (std::string_view program : { "logname", "tty", "arch", "nproc", "jq",
             "du", "ps", "lsblk", "objdump", "env", "rg", "ip", "ldd" }) {
        CHECK_FALSE(shell_builtin_allowed(program));
    }
    const ShellAnalysis with_operand = analyze_shell("tree node_modules");
    CHECK(shell_builtin_allowed(with_operand.invocations.front().program));
    CHECK_FALSE(shell_readonly_allowed(with_operand.invocations.front()));
#else
    for (std::string_view program :
        { "whoami", "logname", "tty", "arch", "nproc", "diff", "uniq", "comm",
            "join", "paste", "fold", "nl", "rev", "tac", "od", "base64", "jq",
            "sha1sum", "sha512sum", "cksum", "which", "type", "whereis", "df",
            "du", "free", "ps", "pgrep", "lsof", "ss", "netstat", "uptime",
            "who", "w", "last", "getent", "lsblk", "lscpu", "objdump",
            "readelf", "nm", "seq", "expr", "sleep" }) {
        CHECK(shell_builtin_allowed(program));
    }
    for (std::string_view program : { "xxd", "iconv", "sort", "date", "dmesg",
             "env", "rg", "tree", "ip", "ldd" }) {
        CHECK_FALSE(shell_builtin_allowed(program));
    }
    const ShellAnalysis with_operand = analyze_shell("du -sh node_modules");
    CHECK(shell_builtin_allowed(with_operand.invocations.front().program));
    CHECK_FALSE(shell_readonly_allowed(with_operand.invocations.front()));
#endif
}

TEST_CASE("application state shares grants with children")
{
    auto parent = imza::test::make_test_state();
    const std::vector<PermissionGrant> grants { ExternalGrant {
        std::filesystem::current_path().lexically_normal() } };
    REQUIRE(parent->permissions->install(grants));
    auto child = imza::make_child_application_state(
        *parent, imza::test::run_immediately);
    CHECK(parent->permissions == child->permissions);
    CHECK(child->permissions->snapshot()->size() == 1);
    CHECK(parent->runtime_flags == child->runtime_flags);
    CHECK(parent->environment == child->environment);
    CHECK(parent->environment->system() == child->environment->system());
}

TEST_CASE("session lifecycle clears grants only after successful activation")
{
    PermissionFixture fixture;
    auto state       = imza::test::make_test_state();
    const auto grant = ExternalGrant { fixture.outside };
    REQUIRE(state->permissions->install({ grant }));
    state->session->set_title("Current session");

    enqueue_user_modal(*state, SessionsModal { });
    resolve_modal(*state, fixture.root() / "missing-session.json");
    CHECK(state->session->title() == "Current session");
    CHECK(state->permissions->snapshot()->size() == 1);

    const auto path = fixture.root() / "session.json";
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
    auto parent      = imza::test::make_test_state();
    const auto grant = ExternalGrant { fixture.outside };
    REQUIRE(parent->permissions->install({ grant }));

    CHECK(parent->environment->chdir(fixture.root() / "missing")
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

    const auto sibling = fixture.root() / "outside-sibling";
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

TEST_CASE("sensitive paths require an exact grant even inside trusted roots")
{
    PermissionFixture fixture;
    imza::test::write_file(fixture.workspace / ".env", "SECRET=1\n");
    std::filesystem::create_directories(fixture.workspace / ".git");
    imza::test::write_file(fixture.workspace / ".git" / "config", "[core]\n");
    imza::test::write_file(fixture.workspace / "server.pem", "key\n");

    // Trusted-root reads stop being automatic.
    for (const auto& target : std::vector<std::filesystem::path> {
             fixture.workspace / ".env", fixture.workspace / ".git" / "config",
             fixture.workspace / "server.pem" }) {
        const auto read = evaluate_filesystem_request(
            read_request(target), fixture.context(Session::Mode::BUILD));
        CHECK(read.decision.kind == PermissionDecision::Kind::ASK);
    }

    // A workspace-wide grant does not inherit down into secret material.
    const PermissionGrant directory = ExternalGrant { fixture.workspace };
    const auto granted_directory
        = evaluate_filesystem_request(read_request(fixture.workspace / ".env"),
            fixture.context(Session::Mode::BUILD, { directory }));
    CHECK(granted_directory.decision.kind == PermissionDecision::Kind::ASK);

    const PermissionGrant exact = ExternalGrant { fixture.workspace / ".env" };
    const auto granted_exact
        = evaluate_filesystem_request(read_request(fixture.workspace / ".env"),
            fixture.context(Session::Mode::PLAN, { exact }));
    CHECK(granted_exact.decision.kind == PermissionDecision::Kind::ACCEPT);

    // Writes keep the Plan-mode rejection ahead of any grant.
    const auto planned_write
        = evaluate_filesystem_request(write_request(fixture.workspace / ".env"),
            fixture.context(Session::Mode::PLAN, { exact }));
    CHECK(planned_write.decision.kind == PermissionDecision::Kind::REJECT);

    // A non-sensitive neighbor keeps its normal trusted-root verdict.
    const auto neighbor = evaluate_filesystem_request(
        read_request(fixture.workspace / "inside.txt"),
        fixture.context(Session::Mode::PLAN));
    CHECK(neighbor.decision.kind == PermissionDecision::Kind::ACCEPT);
}

TEST_CASE("approving a sensitive file grants the file, not its directory")
{
    PermissionFixture fixture;
    imza::test::write_file(fixture.workspace / ".env", "SECRET=1\n");
    imza::test::write_file(fixture.workspace / ".env.local", "SECRET=2\n");

    const auto ask
        = evaluate_filesystem_request(read_request(fixture.workspace / ".env"),
            fixture.context(Session::Mode::PLAN));
    REQUIRE(ask.decision.kind == PermissionDecision::Kind::ASK);
    REQUIRE(ask.request.has_value());
    const auto grant = filesystem_session_grant(*ask.request);
    REQUIRE(grant.has_value());
    CHECK(*grant == fixture.workspace / ".env");

    const auto again = evaluate_filesystem_request(
        read_request(fixture.workspace / ".env"),
        fixture.context(Session::Mode::PLAN, { PermissionGrant { *grant } }));
    CHECK(again.decision.kind == PermissionDecision::Kind::ACCEPT);

    const auto sibling = evaluate_filesystem_request(
        read_request(fixture.workspace / ".env.local"),
        fixture.context(Session::Mode::PLAN, { PermissionGrant { *grant } }));
    CHECK(sibling.decision.kind == PermissionDecision::Kind::ASK);
}

TEST_CASE("sensitive removal and copy follow the exact-grant rule")
{
    PermissionFixture fixture;
    imza::test::write_file(fixture.workspace / ".env", "SECRET=1\n");
    imza::test::write_file(fixture.outside / "id_rsa", "key\n");
    const PermissionGrant directory = ExternalGrant { fixture.workspace };

    // Remove keeps its always-ask class; a directory grant does not help.
    const auto removal = evaluate_filesystem_request(
        remove_request(fixture.workspace / ".env"),
        fixture.context(Session::Mode::BUILD, { directory }));
    CHECK(removal.decision.kind == PermissionDecision::Kind::ASK);

    // Copying onto a sensitive destination asks despite the grant.
    const auto onto_secret = evaluate_filesystem_request(
        CopyFileRequest {
            fixture.workspace / "inside.txt", fixture.workspace / ".env" },
        fixture.context(Session::Mode::BUILD, { directory }));
    CHECK(onto_secret.decision.kind == PermissionDecision::Kind::ASK);
}

TEST_CASE("sensitive path classification matches the credential patterns")
{
    CHECK(path_is_sensitive(".env"));
    CHECK(path_is_sensitive(".env.production"));
    CHECK(path_is_sensitive("config/server.pem"));
    CHECK(path_is_sensitive("keys/id_rsa"));
    CHECK(path_is_sensitive("ID_ECDSA"));
    CHECK(path_is_sensitive("credentials.json"));
    CHECK(path_is_sensitive(".git/config"));
    CHECK(path_is_sensitive(".git-credentials"));

    CHECK_FALSE(path_is_sensitive("environment"));
    CHECK_FALSE(path_is_sensitive("notes.envx"));
    CHECK_FALSE(path_is_sensitive("keyring.txt"));
    CHECK_FALSE(path_is_sensitive("src/credentials_reader.cpp"));

    CHECK(command_touches_sensitive("cat .env"));
    CHECK(command_touches_sensitive("git -C repo/.git status"));
    CHECK(command_touches_sensitive("VAR=.env printenv"));
    CHECK_FALSE(command_touches_sensitive("git status"));
    CHECK_FALSE(command_touches_sensitive("cat inside.txt"));
}

TEST_CASE("shell commands naming sensitive material always ask")
{
    PermissionFixture fixture;
    imza::test::write_file(fixture.workspace / ".env", "SECRET=1\n");

    const auto cat
        = evaluate_shell_request({ "cat .env", std::chrono::seconds(10), { } },
            fixture.context(Session::Mode::BUILD));
    CHECK(cat.decision.kind == PermissionDecision::Kind::ASK);
    CHECK(cat.session_grants.empty());

    // A session grant for the same program is no better.
    const PermissionGrant shell_grant = ShellCommandGrant { "cat" };
    const auto granted
        = evaluate_shell_request({ "cat .env", std::chrono::seconds(10), { } },
            fixture.context(Session::Mode::BUILD, { shell_grant }));
    CHECK(granted.decision.kind == PermissionDecision::Kind::ASK);
    CHECK(granted.session_grants.empty());

    // Non-sensitive operands keep their catalog verdict.
    const auto plain = evaluate_shell_request(
        { "cat inside.txt", std::chrono::seconds(10), { } },
        fixture.context(Session::Mode::BUILD));
    CHECK(plain.decision.kind == PermissionDecision::Kind::ACCEPT);
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

    // Missing parents are fine: save_text creates every level, so only a
    // non-directory ancestor can block the write.
    const auto missing_parent = evaluate_filesystem_request(
        write_request(fixture.workspace / "absent" / "new.txt"),
        fixture.context(Session::Mode::BUILD));
    CHECK(missing_parent.decision.kind == PermissionDecision::Kind::ACCEPT);
    REQUIRE(missing_parent.request.has_value());

    const auto file_parent = evaluate_filesystem_request(
        write_request(fixture.workspace / "inside.txt" / "new.txt"),
        fixture.context(Session::Mode::BUILD));
    CHECK(file_parent.decision.kind == PermissionDecision::Kind::REJECT);
}

TEST_CASE("filesystem normalization follows symlinked targets and parents")
{
    if (!imza::test::can_create_symlinks()) {
        SKIP_TEST("this account cannot create symlinks");
    }
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
}

TEST_CASE("central evaluator assigns explicit policies to built-in tools")
{
    PermissionFixture fixture;
    const PermissionContext plan = fixture.context(Session::Mode::PLAN);
    const Config config;
    const std::vector<Skill> skills;
    Session loaded;

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
    CHECK(evaluate("load_module", R"({"name":"canvas"})").decision.kind
        == PermissionDecision::Kind::ACCEPT);
    CHECK(evaluate("load_module", R"({"name":""})").decision.kind
        == PermissionDecision::Kind::REJECT);
    CHECK(evaluate("load_module", "{}").decision.kind
        == PermissionDecision::Kind::REJECT);
}

TEST_CASE("shell policy reuses, combines, and broadens session grants")
{
    PermissionFixture fixture;
    const Config config;
    const std::vector<Skill> skills;
    Session loaded;
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

    const ShellEvaluation redirected = evaluate("ls > files.txt");
    CHECK(redirected.decision.kind == PermissionDecision::Kind::ASK);
    CHECK(redirected.session_grants.empty());
    CHECK(redirected.request.command == "ls > files.txt");

    // A program-wide grant covers the program, not the options that make
    // it read configuration or run another program.
    const PermissionStore::Grants everything { ShellCommandGrant {
        "git", std::nullopt } };
    for (std::string_view command : { "git -c core.pager=evil log",
             "git -c core.fsmonitor=touch /tmp/x status",
             "git --exec-path=/tmp/e log", "git --git-dir=/tmp/x log",
             "git --config-env=a=b log" }) {
        const ShellEvaluation denied
            = evaluate(std::string(command), everything);
        CHECK(denied.decision.kind == PermissionDecision::Kind::ASK);
        CHECK(denied.session_grants.empty());
    }
    CHECK(evaluate("git log", everything).decision.kind
        == PermissionDecision::Kind::ACCEPT);
    CHECK(evaluate("git -C build status", everything).decision.kind
        == PermissionDecision::Kind::ACCEPT);
}

TEST_CASE("shell policy rejects an empty command")
{
    PermissionFixture fixture;
    const auto empty = shell_policy("", fixture.context(Session::Mode::BUILD));
    CHECK(empty.decision.kind == PermissionDecision::Kind::REJECT);
}

TEST_CASE("launcher wrapping cannot bypass the shell rules")
{
    PermissionFixture fixture;
    const auto evaluate = [&](std::string command) {
        return shell_policy(
            std::move(command), fixture.context(Session::Mode::BUILD));
    };

    const ShellEvaluation deleted = evaluate("bash -c 'rm -rf build'");
    CHECK(deleted.decision.kind == PermissionDecision::Kind::REJECT);
    CHECK(deleted.decision.reason.find("imza.fs.remove") != std::string::npos);

    const ShellEvaluation prefixed = evaluate("env FOO=bar rm -rf build");
    CHECK(prefixed.decision.kind == PermissionDecision::Kind::REJECT);

    // Unwrapping works on every host, but the accept verdicts below rely
    // on the POSIX read-only catalog; the Windows catalog (cmd.exe
    // builtins) has no ls, so the launcher-wrapped forms legitimately ask.
#ifdef _WIN32
    // dir is catalogued read-only on Windows, so both launcher-wrapped and
    // direct forms are accepted; the wrapper cannot smuggle anything past
    // the same rules.
    CHECK(evaluate("bash -c 'dir'").decision.kind
        == PermissionDecision::Kind::ACCEPT);
    CHECK(evaluate("cmd /c dir").decision.kind
        == PermissionDecision::Kind::ACCEPT);
#else
    CHECK(evaluate("bash -c 'ls'").decision.kind
        == PermissionDecision::Kind::ACCEPT);
    CHECK(evaluate("bash -c 'git status'").decision.kind
        == PermissionDecision::Kind::ACCEPT);
    CHECK(evaluate("env FOO=bar ls").decision.kind
        == PermissionDecision::Kind::ACCEPT);
    CHECK(
        evaluate("sudo ls").decision.kind == PermissionDecision::Kind::ACCEPT);
#endif

    // Unreadable code asks once and carries no grant, so approval cannot
    // become a reusable bypass.
    for (std::string_view command : { "python3 -c 'print(1)'", "bash script.sh",
             "awk 'BEGIN{system(\"ls\")}'" }) {
        const ShellEvaluation result = evaluate(std::string(command));
        CHECK(result.decision.kind == PermissionDecision::Kind::ASK);
        CHECK(result.session_grants.empty());
    }
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
    Session loaded;
    Config config;
    const ToolCallRequest request { "load_skill", R"({"name":"docs"})", "",
        "" };

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
    REQUIRE(loaded.load_skill(skill, error));
    CHECK(evaluate_tool_request(request, fixture.context(Session::Mode::PLAN),
              config, skills, loaded)
              .decision.kind
        == PermissionDecision::Kind::ACCEPT);

    const Skill missing { "missing", "Missing instructions",
        fixture.outside / "missing.md", Skill::Scope::GLOBAL, std::nullopt };
    CHECK(
        evaluate_tool_request({ "load_skill", R"({"name":"missing"})", "", "" },
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
    CHECK(evaluate_tool_request({ "load_skill", R"({"name":"large"})", "", "" },
              fixture.context(Session::Mode::PLAN), config,
              std::vector<Skill> { large }, loaded)
              .decision.kind
        == PermissionDecision::Kind::REJECT);
}

TEST_CASE("bundled skills are always allowed and deny policy cannot hide them")
{
    PermissionFixture fixture;
    const auto path = fixture.outside / "SKILL.md";
    {
        std::ofstream file(path);
        file << "Use the documented workflow.";
    }
    const Skill skill { "imza-docs", "Bundled manual", path,
        Skill::Scope::GLOBAL, std::nullopt, true };
    const std::vector<Skill> skills { skill };
    Session loaded;
    Config config;
    const ToolCallRequest request { "load_skill", R"({"name":"imza-docs"})", "",
        "" };

    config.global_skills["imza-docs"] = SkillPolicy::DENY;
    auto evaluation                   = evaluate_tool_request(
        request, fixture.context(Session::Mode::PLAN), config, skills, loaded);
    CHECK(evaluation.decision.kind == PermissionDecision::Kind::ACCEPT);

    // Bundled skill bodies are application content: reads inside the
    // skill directory never prompt, even without a session grant.
    loaded.clear_loaded_skills();
    std::string load_error;
    REQUIRE(loaded.load_skill(skill, load_error));
    const auto skill_directories = loaded.loaded_skill_directories();
    REQUIRE_FALSE(skill_directories.empty());
    const PermissionContext context
        = fixture.context(Session::Mode::PLAN, { }, skill_directories);
    const auto read = evaluate_filesystem_request(read_request(path), context);
    CHECK(read.decision.kind == PermissionDecision::Kind::ACCEPT);
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

    imza::JsonValue bound = imza::JsonValue::object_t { };
    {
        auto& object    = bound.get<imza::JsonValue::object_t>();
        object["name"]  = imza::JsonValue("docs");
        object["scope"] = imza::JsonValue("global");
        object["path"]  = imza::JsonValue(canonical);
    }
    CHECK(authorized_skill_path(
              skill, { "load_skill", imza::json_dump(bound), "", "" })
        == canonical_skill_path(skill));

    imza::JsonValue other = imza::JsonValue::object_t { };
    {
        auto& object    = other.get<imza::JsonValue::object_t>();
        object["name"]  = imza::JsonValue("docs");
        object["scope"] = imza::JsonValue("global");
        object["path"] = JsonValue((fixture.outside / "elsewhere.md").string());
    }
    CHECK_FALSE(authorized_skill_path(
        skill, { "load_skill", imza::json_dump(other), "", "" }));

    CHECK_FALSE(authorized_skill_path(
        skill, { "load_skill", R"({"name":"docs"})", "", "" }));
}

TEST_CASE("mkdir follows the write policy, remove always asks")
{
    PermissionFixture fixture;

    // mkdir is write-class: rejected in Plan, automatic inside the workspace
    // in Build, asking outside.
    const auto planned = evaluate_filesystem_request(
        mkdir_request(fixture.workspace / "new-dir"),
        fixture.context(Session::Mode::PLAN));
    CHECK(planned.decision.kind == PermissionDecision::Kind::REJECT);

    const auto built = evaluate_filesystem_request(
        mkdir_request(fixture.workspace / "new-dir"),
        fixture.context(Session::Mode::BUILD));
    CHECK(built.decision.kind == PermissionDecision::Kind::ACCEPT);
    REQUIRE(built.request.has_value());
    CHECK(filesystem_target(*built.request) == fixture.workspace / "new-dir");

    const auto outside = evaluate_filesystem_request(
        mkdir_request(fixture.outside / "new-dir"),
        fixture.context(Session::Mode::BUILD));
    CHECK(outside.decision.kind == PermissionDecision::Kind::ASK);

    const auto inside_remove = evaluate_filesystem_request(
        remove_request(fixture.workspace / "inside.txt"),
        fixture.context(Session::Mode::BUILD));
    CHECK(inside_remove.decision.kind == PermissionDecision::Kind::ASK);
    CHECK(inside_remove.decision.reason.find("removing") != std::string::npos);

    const PermissionGrant directory = ExternalGrant { fixture.workspace };
    const auto granted              = evaluate_filesystem_request(
        remove_request(fixture.workspace / "inside.txt"),
        fixture.context(Session::Mode::BUILD, { directory }));
    CHECK(granted.decision.kind == PermissionDecision::Kind::ACCEPT);
}

TEST_CASE("remove rejects non-empty directories and session roots")
{
    PermissionFixture fixture;
    std::filesystem::create_directories(fixture.workspace / "full");
    std::filesystem::create_directories(fixture.workspace / "empty");
    imza::test::write_file(fixture.workspace / "full" / "child.txt", "x\n");

    const auto non_empty = evaluate_filesystem_request(
        remove_request(fixture.workspace / "full"),
        fixture.context(Session::Mode::BUILD));
    CHECK(non_empty.decision.kind == PermissionDecision::Kind::REJECT);
    CHECK(non_empty.decision.reason.find("not empty") != std::string::npos);

    const auto empty = evaluate_filesystem_request(
        remove_request(fixture.workspace / "empty"),
        fixture.context(Session::Mode::BUILD));
    CHECK(empty.decision.kind == PermissionDecision::Kind::ASK);

    // The working directory, the project root, and their ancestors are never
    // removable, even with a grant covering them.
    const PermissionGrant grant = ExternalGrant { fixture.root() };
    for (const auto& target : { fixture.workspace, fixture.root() }) {
        const auto result = evaluate_filesystem_request(remove_request(target),
            fixture.context(Session::Mode::BUILD, { grant }));
        CHECK(result.decision.kind == PermissionDecision::Kind::REJECT);
        CHECK(result.decision.reason.find("session root") != std::string::npos);
    }

    const auto missing = evaluate_filesystem_request(
        remove_request(fixture.workspace / "absent.txt"),
        fixture.context(Session::Mode::BUILD));
    CHECK(missing.decision.kind == PermissionDecision::Kind::REJECT);
    CHECK(missing.decision.reason.find("does not exist") != std::string::npos);
}

TEST_CASE("shell deletion programs are rejected with binding guidance")
{
    PermissionFixture fixture;
    const PermissionStore::Grants grants { PermissionGrant {
        ShellCommandGrant { "rm", std::nullopt } } };
    const ShellEvaluation denied = shell_policy(
        "rm -rf build", fixture.context(Session::Mode::BUILD, grants));
    CHECK(denied.decision.kind == PermissionDecision::Kind::REJECT);
    CHECK(denied.decision.reason.find("'rm' is not permitted")
        != std::string::npos);
    CHECK(denied.decision.reason.find("imza.fs.remove") != std::string::npos);

    for (std::string_view program :
        { "rmdir", "del", "erase", "unlink", "shred" }) {
        CHECK(shell_deletion_denied(program));
        const ShellEvaluation result
            = shell_policy(std::string(program) + " target",
                fixture.context(Session::Mode::BUILD, grants));
        CHECK(result.decision.kind == PermissionDecision::Kind::REJECT);
        CHECK(
            result.decision.reason.find("imza.fs.remove") != std::string::npos);
    }
    CHECK_FALSE(shell_deletion_denied("ls"));
    CHECK_FALSE(shell_deletion_denied("git"));
}

TEST_CASE("skill evaluation binds approval to the canonical instruction path")
{
    if (!imza::test::can_create_symlinks()) {
        SKIP_TEST("this account cannot create symlinks");
    }
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
    Session loaded;
    const ToolCallRequest request { "load_skill", R"({"name":"changing"})", "",
        "" };
    const PermissionEvaluation approved = evaluate_tool_request(
        request, fixture.context(Session::Mode::PLAN), config, skills, loaded);
    REQUIRE(approved.decision.kind == PermissionDecision::Kind::ASK);
    const imza::JsonValue parsed_args = imza::parse_json(approved.request.args);
    const imza::JsonValue* bound_path = imza::find_member(parsed_args, "path");
    REQUIRE(bound_path != nullptr);
    REQUIRE(bound_path->is_string());
    CHECK(bound_path->as<std::string>() == first.string());

    std::filesystem::remove(link);
    std::filesystem::create_symlink(second, link);
    const PermissionEvaluation current = evaluate_tool_request(approved.request,
        fixture.context(Session::Mode::PLAN), config, skills, loaded);

    REQUIRE(current.decision.kind == PermissionDecision::Kind::ASK);
    CHECK(current.request.args != approved.request.args);
    const imza::JsonValue parsed_current
        = imza::parse_json(current.request.args);
    const imza::JsonValue* current_path
        = imza::find_member(parsed_current, "path");
    REQUIRE(current_path != nullptr);
    REQUIRE(current_path->is_string());
    CHECK(current_path->as<std::string>() == second.string());
}

TEST_CASE("load_mcp policy: connected accepts, offline asks, disabled and "
          "failed reject")
{
    PermissionFixture fixture;
    imza::test::LoopbackMcpServer server;
    REQUIRE(server.start());

    imza::McpServerConfig disabled = server_config("idle", server.url());
    disabled.enabled               = false;
    imza::McpSubsystem manager({ { "exa", server_config("exa", server.url()) },
        { "offline", server_config("offline", "http://127.0.0.1:1/mcp") },
        { "idle", disabled },
        { "dead", server_config("dead", "http://127.0.0.1:1/mcp") } });
    manager.connect("exa");
    manager.connect("dead");
    REQUIRE(wait_state(manager, "exa", imza::McpServerState::CONNECTED));
    REQUIRE(wait_state(manager, "dead", imza::McpServerState::FAILED));

    const Config config;
    const std::vector<Skill> skills;
    Session loaded;
    const auto evaluate
        = [&](const std::string& id, PermissionStore::Grants grants = { }) {
              return evaluate_tool_request(
                  { "load_mcp", R"json({"server":")json" + id + R"json("})json",
                      "", "" },
                  fixture.context(
                      Session::Mode::PLAN, std::move(grants), { }, &manager),
                  config, skills, loaded);
          };

    CHECK(evaluate("exa").decision.kind == PermissionDecision::Kind::ACCEPT);

    const auto offline = evaluate("offline");
    REQUIRE(offline.decision.kind == PermissionDecision::Kind::ASK);
    REQUIRE(offline.prompt.has_value());
    CHECK(offline.prompt->allow_for_session == false);
    CHECK(offline.session_grants.empty());
    const auto* request
        = std::get_if<imza::McpRequest>(&offline.prompt->request);
    REQUIRE(request != nullptr);
    CHECK(request->id == "offline");

    CHECK(evaluate("offline", { PermissionGrant { McpGrant { "offline" } } })
              .decision.kind
        == PermissionDecision::Kind::ACCEPT);

    CHECK(evaluate("idle").decision.kind == PermissionDecision::Kind::REJECT);
    const auto failed = evaluate("dead");
    CHECK(failed.decision.kind == PermissionDecision::Kind::REJECT);
    CHECK(failed.decision.reason.find("failed") != std::string::npos);

    CHECK(evaluate("nope").decision.kind == PermissionDecision::Kind::ACCEPT);
    CHECK(evaluate_tool_request({ "load_mcp", R"({"server":""})", "", "" },
              fixture.context(Session::Mode::PLAN), config, skills, loaded)
              .decision.kind
        == PermissionDecision::Kind::REJECT);

    server.stop();
}

TEST_CASE("permission store covers McpGrant by exact id")
{
    PermissionStore store;
    REQUIRE(store.install({ McpGrant { "exa" } }));
    CHECK(grants_cover(store.snapshot(), McpGrant { "exa" }));
    CHECK_FALSE(grants_cover(store.snapshot(), McpGrant { "other" }));
    CHECK_FALSE(store.install({ McpGrant { "" } }));
    REQUIRE(store.install({ McpGrant { "exa" } }));
    CHECK(store.snapshot()->size() == 1);
}

TEST_CASE("a loaded skill implies read-class access inside its directory")
{
    PermissionFixture fixture;
    const auto skill_dir = fixture.root() / "skill-pack";
    std::filesystem::create_directories(skill_dir / "scripts");
    imza::test::write_file(skill_dir / "SKILL.md", "instructions");
    imza::test::write_file(skill_dir / "scripts" / "helper.py", "# helper");

    const auto read_decision
        = [&](Session::Mode mode, const std::filesystem::path& target,
              std::vector<std::filesystem::path> dirs) {
              return evaluate_filesystem_request(
                  ReadFileRequest { target }, fixture.context(mode, { }, dirs));
          };

    for (const Session::Mode mode :
        { Session::Mode::PLAN, Session::Mode::BUILD }) {
        CHECK(read_decision(mode, skill_dir / "SKILL.md", { skill_dir })
                  .decision.kind
            == PermissionDecision::Kind::ACCEPT);
        CHECK(read_decision(
                  mode, skill_dir / "scripts" / "helper.py", { skill_dir })
                  .decision.kind
            == PermissionDecision::Kind::ACCEPT);
    }

    CHECK(read_decision(Session::Mode::PLAN, skill_dir / "SKILL.md", { })
              .decision.kind
        == PermissionDecision::Kind::ASK);

    CHECK(read_decision(Session::Mode::PLAN, fixture.outside / "outside.txt",
              { skill_dir })
              .decision.kind
        == PermissionDecision::Kind::ASK);

    // Mutation inside the skill directory still follows ordinary policy.
    const PermissionContext build
        = fixture.context(Session::Mode::BUILD, { }, { skill_dir });
    CHECK(evaluate_filesystem_request(
              WriteFileRequest { skill_dir / "SKILL.md", "x" }, build)
              .decision.kind
        == PermissionDecision::Kind::ASK);
    CHECK(evaluate_filesystem_request(
              RemoveRequest { skill_dir / "SKILL.md" }, build)
              .decision.kind
        == PermissionDecision::Kind::ASK);
}

} // namespace imza
TEST_CASE("child states share the roots process and config owners")
{
    imza::test::IsolatedDataHome home;
    auto state = imza::test::make_test_state();
    imza::test::wait_for_environment(*state);

    auto child = imza::make_child_application_state(
        *state, imza::test::run_immediately);

    CHECK(child->bg_processes == state->bg_processes);
    CHECK(child->config == state->config);
    CHECK(child->providers == state->providers);
    CHECK(child->environment == state->environment);
    CHECK(child->permissions == state->permissions);

    CHECK(child->session != state->session);
    CHECK(child->runner != state->runner);

    // A child renders no review pane and spawns no subagents of its own.
    CHECK(child->review == nullptr);
    CHECK(child->subagents == nullptr);
}
