#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

#include "agent/application_state.h"
#include "agent/flows.h"
#include "agent/permissions.h"
#include "agent/tools.h"
#include "network/json_io.h"
#include "subsystems/environment.h"
#include "subsystems/permission_store.h"

namespace ursa {

namespace {

    class PermissionFixture {
    public:
        PermissionFixture()
        {
            const auto stamp
                = std::chrono::steady_clock::now().time_since_epoch().count();
            root = std::filesystem::temp_directory_path()
                / ("ursa-permission-test-" + std::to_string(stamp));
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
            environment = std::make_shared<WorkspaceEnvironment>(workspace);
            environment->project_root = workspace;
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

    std::string path_args(
        std::string_view key, const std::filesystem::path& path)
    {
        Json::Value value(Json::objectValue);
        value[std::string(key)] = path.string();
        return write_json(value);
    }

} // namespace

TEST_CASE("runtime flags combine independently")
{
    int flags = RuntimeFlag::WEB | RuntimeFlag::SKIP_PERMISSIONS;
    CHECK((flags & RuntimeFlag::WEB) != RuntimeFlag::NONE);
    CHECK((flags & RuntimeFlag::SKIP_PERMISSIONS) != RuntimeFlag::NONE);
    CHECK((flags & RuntimeFlag::SHELL) == RuntimeFlag::NONE);

    flags &= ~RuntimeFlag::WEB;
    CHECK((flags & RuntimeFlag::WEB) == RuntimeFlag::NONE);
    CHECK((flags & RuntimeFlag::SKIP_PERMISSIONS) != RuntimeFlag::NONE);

    const RuntimeFlag interactive = interactive_runtime_flags();
    CHECK((interactive & RuntimeFlag::WEB) != RuntimeFlag::NONE);
    CHECK((interactive & RuntimeFlag::SHELL) != RuntimeFlag::NONE);
    CHECK((interactive & RuntimeFlag::ATTENDED) != RuntimeFlag::NONE);
    CHECK((interactive & RuntimeFlag::SKIP_PERMISSIONS) == RuntimeFlag::NONE);
}

TEST_CASE("permission store installs complete valid sets atomically")
{
    PermissionStore store;
    const auto empty_snapshot = store.snapshot();
    const std::filesystem::path root
        = std::filesystem::temp_directory_path().lexically_normal();
    const std::vector<PermissionGrant> valid {
        PathGrant {
            PathGrant::Access::READ, PathGrant::Target::DIRECTORY, root },
        ShellCommandGrant {
            "git", { "status" }, ShellCommandGrant::Match::EXACT, root },
    };
    CHECK(store.install(valid));
    CHECK(empty_snapshot->empty());
    const auto installed_snapshot = store.snapshot();
    CHECK(store.size() == 2);
    CHECK(store.matches(PathGrant { PathGrant::Access::READ,
        PathGrant::Target::FILE, root / "child" / "file" }));
    CHECK(store.matches(ShellCommandGrant {
        "git", { "status" }, ShellCommandGrant::Match::EXACT, root }));
    CHECK(store.install({ PathGrant { PathGrant::Access::READ,
        PathGrant::Target::FILE, root / "child" / "file" } }));
    CHECK(store.size() == 2);
    CHECK(installed_snapshot->size() == 2);

    const std::vector<PermissionGrant> invalid {
        SkillGrant { root / "skill" },
        PathGrant {
            PathGrant::Access::WRITE, PathGrant::Target::FILE, "relative" },
    };
    CHECK_FALSE(store.install(invalid));
    CHECK(store.size() == 2);
    store.clear();
    CHECK(store.size() == 0);
}

TEST_CASE("global-style shell prefixes and exact runtime commands differ")
{
    PermissionStore store;
    const auto root = std::filesystem::temp_directory_path().lexically_normal();
    const std::vector<PermissionGrant> grants { PermissionGrant {
        ShellCommandGrant {
            "git", { "status" }, ShellCommandGrant::Match::PREFIX, root } } };
    REQUIRE(store.install(grants));
    CHECK(store.matches(ShellCommandGrant { "git", { "status", "--short" },
        ShellCommandGrant::Match::EXACT, root }));
    CHECK_FALSE(store.matches(ShellCommandGrant {
        "git", { "diff" }, ShellCommandGrant::Match::EXACT, root }));
    CHECK(store.install({ ShellCommandGrant { "git", { "status", "--short" },
        ShellCommandGrant::Match::EXACT, root } }));
    CHECK(store.size() == 1);
}

TEST_CASE("application state shares grants with children")
{
    const auto immediate = [](std::function<void()> task) { task(); };
    auto parent          = make_application_state(immediate, Config { });
    const std::vector<PermissionGrant> grants { PermissionGrant {
        PathGrant { PathGrant::Access::READ, PathGrant::Target::FILE,
            (std::filesystem::current_path() / "shared.txt")
                .lexically_normal() } } };
    REQUIRE(parent->permissions->install(grants));
    auto child = make_child_application_state(*parent, immediate);
    CHECK(parent->permissions == child->permissions);
    CHECK(child->permissions->size() == 1);
    CHECK(parent->runtime_flags == child->runtime_flags);
    CHECK(parent->environment == child->environment);
    CHECK(parent->environment->system() == child->environment->system());
}

TEST_CASE("filesystem policy distinguishes Plan and Build writes")
{
    PermissionFixture fixture;
    const std::string args
        = path_args("file_path", fixture.workspace / "new.txt");

    const auto planned = evaluate_filesystem_request(
        "write", args, fixture.context(Session::Mode::PLAN));
    CHECK(planned.decision.kind == PermissionDecision::Kind::ASK);
    REQUIRE(planned.request.has_value());
    CHECK(planned.request->target == fixture.workspace / "new.txt");
    REQUIRE(filesystem_session_grants(*planned.request).size() == 1);

    const auto built = evaluate_filesystem_request(
        "write", args, fixture.context(Session::Mode::BUILD));
    CHECK(built.decision.kind == PermissionDecision::Kind::ACCEPT);
}

TEST_CASE("filesystem policy trusts only configured roots")
{
    PermissionFixture fixture;
    REQUIRE(fixture.environment->working_directory == fixture.workspace);
    REQUIRE(fixture.environment->project_root == fixture.workspace);
    REQUIRE(fixture.system->temporary_directory == fixture.temporary);
    const auto workspace_read = evaluate_filesystem_request("read",
        path_args("path", fixture.workspace / "inside.txt"),
        fixture.context(Session::Mode::PLAN));
    CHECK(workspace_read.decision.kind == PermissionDecision::Kind::ACCEPT);

    const auto temporary_read = evaluate_filesystem_request("read",
        path_args("path", fixture.temporary / "temporary.txt"),
        fixture.context(Session::Mode::PLAN));
    CHECK(temporary_read.decision.kind == PermissionDecision::Kind::ACCEPT);

    const auto outside_read = evaluate_filesystem_request("read",
        path_args("path", fixture.outside / "outside.txt"),
        fixture.context(Session::Mode::PLAN));
    CHECK(outside_read.decision.kind == PermissionDecision::Kind::ASK);

    const auto outside_list = evaluate_filesystem_request("list",
        path_args("path", fixture.outside),
        fixture.context(Session::Mode::PLAN));
    CHECK(outside_list.decision.kind == PermissionDecision::Kind::ACCEPT);
}

TEST_CASE("filesystem policy falls back to the working directory")
{
    PermissionFixture fixture;
    fixture.environment->project_root.reset();

    const auto inside  = evaluate_filesystem_request("read",
        path_args("path", fixture.workspace / "inside.txt"),
        fixture.context(Session::Mode::PLAN));
    const auto outside = evaluate_filesystem_request("read",
        path_args("path", fixture.outside / "outside.txt"),
        fixture.context(Session::Mode::PLAN));

    CHECK(inside.decision.kind == PermissionDecision::Kind::ACCEPT);
    CHECK(outside.decision.kind == PermissionDecision::Kind::ASK);
}

TEST_CASE("filesystem grants are exact unless a directory is granted")
{
    PermissionFixture fixture;
    const auto target           = fixture.outside / "outside.txt";
    const PermissionGrant exact = PathGrant { PathGrant::Access::READ,
        PathGrant::Target::FILE, target };
    const auto accepted
        = evaluate_filesystem_request("read", path_args("path", target),
            fixture.context(Session::Mode::PLAN, { exact }));
    CHECK(accepted.decision.kind == PermissionDecision::Kind::ACCEPT);

    const PermissionGrant directory = PathGrant { PathGrant::Access::READ,
        PathGrant::Target::DIRECTORY, fixture.outside };
    const auto recursive
        = evaluate_filesystem_request("read", path_args("path", target),
            fixture.context(Session::Mode::PLAN, { directory }));
    CHECK(recursive.decision.kind == PermissionDecision::Kind::ACCEPT);
}

TEST_CASE("filesystem normalization rejects malformed and wrong-type targets")
{
    PermissionFixture fixture;
    const auto malformed = evaluate_filesystem_request(
        "read", "{}", fixture.context(Session::Mode::PLAN));
    CHECK(malformed.decision.kind == PermissionDecision::Kind::REJECT);

    const auto wrong_type = evaluate_filesystem_request("read",
        path_args("path", fixture.workspace),
        fixture.context(Session::Mode::PLAN));
    CHECK(wrong_type.decision.kind == PermissionDecision::Kind::REJECT);

    const auto relative = evaluate_filesystem_request("read",
        R"({"path":"inside.txt"})", fixture.context(Session::Mode::PLAN));
    CHECK(relative.decision.kind == PermissionDecision::Kind::ACCEPT);
    REQUIRE(relative.request.has_value());
    CHECK(relative.request->target == fixture.workspace / "inside.txt");
}

} // namespace ursa
