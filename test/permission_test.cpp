#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "app/application_state.h"
#include "app/flows.h"
#include "conversation/persistence.h"
#include "network/json_io.h"
#include "permissions/filesystem.h"
#include "permissions/store.h"
#include "tools/tool.h"
#include "workspace/environment.h"

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

    std::string path_args(
        std::string_view key, const std::filesystem::path& path)
    {
        Json::Value value(Json::objectValue);
        value[std::string(key)] = path.string();
        return write_json(value);
    }

    std::string edit_args(const std::filesystem::path& path)
    {
        Json::Value value(Json::objectValue);
        value["file_path"]  = path.string();
        value["old_string"] = "content";
        value["new_string"] = "updated";
        return write_json(value);
    }

    std::string write_args(const std::filesystem::path& path)
    {
        Json::Value value(Json::objectValue);
        value["file_path"] = path.string();
        value["text"]      = "updated";
        return write_json(value);
    }

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

TEST_CASE("session loading stages data before activation")
{
    PermissionFixture fixture;
    const auto path = fixture.root / "session.json";
    write_session_file(path, fixture.workspace, "Loaded session");

    LoadedSession loaded;
    REQUIRE(read_session(path, loaded) == Status::OK);
    CHECK(loaded.snapshot.title == "Loaded session");
    CHECK(loaded.workspace == fixture.workspace);

    Session session;
    session.set_title("Current session");
    REQUIRE(load_session(path, session) == Status::OK);
    CHECK(session.title() == "Loaded session");
}

TEST_CASE("session lifecycle clears grants only after successful activation")
{
    PermissionFixture fixture;
    const auto immediate = [](std::function<void()> task) { task(); };
    auto state           = make_application_state(immediate, Config { });
    const auto grant     = PathGrant { PathGrant::Access::READ,
        PathGrant::Target::FILE, fixture.outside / "outside.txt" };
    REQUIRE(state->permissions->install({ grant }));
    state->session->set_title("Current session");

    enqueue_user_modal(*state, SessionsModal { });
    resolve_modal(*state, fixture.root / "missing-session.json");
    CHECK(state->session->title() == "Current session");
    CHECK(state->permissions->size() == 1);

    const auto path = fixture.root / "session.json";
    const auto workspace
        = std::filesystem::weakly_canonical(std::filesystem::current_path());
    write_session_file(path, workspace, "Loaded session");
    enqueue_user_modal(*state, SessionsModal { });
    resolve_modal(*state, path);
    CHECK(state->session->title() == "Loaded session");
    CHECK(state->session->error().empty());
    CHECK(state->permissions->size() == 0);

    REQUIRE(state->permissions->install({ grant }));
    CHECK(save_session(*state->session) == Status::OK);
    CHECK(state->permissions->size() == 1);
    run_slash(*state, "/new");
    CHECK(state->permissions->size() == 0);
}

TEST_CASE("failed directory changes and child creation retain grants")
{
    PermissionFixture fixture;
    const auto immediate = [](std::function<void()> task) { task(); };
    auto parent          = make_application_state(immediate, Config { });
    const auto grant     = PathGrant { PathGrant::Access::READ,
        PathGrant::Target::FILE, fixture.outside / "outside.txt" };
    REQUIRE(parent->permissions->install({ grant }));

    CHECK_FALSE(parent->environment->chdir(fixture.root / "missing"));
    CHECK(parent->permissions->size() == 1);
    auto child = make_child_application_state(*parent, immediate);
    CHECK(child->permissions == parent->permissions);
    CHECK(child->permissions->size() == 1);
}

TEST_CASE("permission store snapshots remain valid during concurrent changes")
{
    PermissionStore store;
    const std::filesystem::path root
        = std::filesystem::temp_directory_path().lexically_normal();
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
                    const auto* path = std::get_if<PathGrant>(&grant);
                    if (path == nullptr || !path->path.is_absolute()) {
                        snapshots_valid.store(false);
                    }
                }
            }
        });
    }
    start.store(true);
    for (int iteration = 0; iteration < 100; ++iteration) {
        REQUIRE(store.install(
            { PathGrant { PathGrant::Access::READ, PathGrant::Target::FILE,
                root / ("permission-" + std::to_string(iteration)) } }));
        if (iteration % 7 == 0) {
            store.clear();
        }
    }
    readers.clear();
    CHECK(snapshots_valid.load());
    store.clear();
    CHECK(store.snapshot()->empty());
}

TEST_CASE("filesystem policy distinguishes Plan and Build writes")
{
    PermissionFixture fixture;
    const std::string args = write_args(fixture.workspace / "new.txt");

    const auto planned = evaluate_filesystem_request(
        "write", args, fixture.context(Session::Mode::PLAN));
    CHECK(planned.decision.kind == PermissionDecision::Kind::ASK);
    REQUIRE(planned.request.has_value());
    CHECK(planned.request->target == fixture.workspace / "new.txt");
    REQUIRE(filesystem_session_grant(*planned.request).has_value());

    const auto built = evaluate_filesystem_request(
        "write", args, fixture.context(Session::Mode::BUILD));
    CHECK(built.decision.kind == PermissionDecision::Kind::ACCEPT);
}

TEST_CASE("filesystem policy covers all trusted and granted write cases")
{
    PermissionFixture fixture;
    const auto workspace_edit = evaluate_filesystem_request("edit",
        edit_args(fixture.workspace / "inside.txt"),
        fixture.context(Session::Mode::PLAN));
    CHECK(workspace_edit.decision.kind == PermissionDecision::Kind::ASK);

    const auto temporary_write = evaluate_filesystem_request("write",
        write_args(fixture.temporary / "new.txt"),
        fixture.context(Session::Mode::BUILD));
    CHECK(temporary_write.decision.kind == PermissionDecision::Kind::ACCEPT);

    const auto outside_write = evaluate_filesystem_request("write",
        write_args(fixture.outside / "new.txt"),
        fixture.context(Session::Mode::BUILD));
    CHECK(outside_write.decision.kind == PermissionDecision::Kind::ASK);

    const PermissionGrant grant = PathGrant { PathGrant::Access::WRITE,
        PathGrant::Target::FILE, fixture.outside / "new.txt" };
    const auto granted          = evaluate_filesystem_request("write",
        write_args(fixture.outside / "new.txt"),
        fixture.context(Session::Mode::PLAN, { grant }));
    CHECK(granted.decision.kind == PermissionDecision::Kind::ACCEPT);
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

TEST_CASE("filesystem policy trusts the repository above a nested cwd")
{
    PermissionFixture fixture;
    const auto nested = fixture.workspace / "nested";
    std::filesystem::create_directories(nested);
    fixture.environment->working_directory = nested;

    const auto result = evaluate_filesystem_request("read",
        R"({"path":"../inside.txt"})", fixture.context(Session::Mode::PLAN));
    CHECK(result.decision.kind == PermissionDecision::Kind::ACCEPT);
    REQUIRE(result.request.has_value());
    CHECK(result.request->target == fixture.workspace / "inside.txt");
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

    const auto sibling = fixture.root / "outside-sibling";
    std::filesystem::create_directories(sibling);
    std::ofstream(sibling / "file.txt") << "content\n";
    const auto sibling_result = evaluate_filesystem_request("read",
        path_args("path", sibling / "file.txt"),
        fixture.context(Session::Mode::PLAN, { directory }));
    CHECK(sibling_result.decision.kind == PermissionDecision::Kind::ASK);
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

TEST_CASE("filesystem normalization resolves traversal and missing writes")
{
    PermissionFixture fixture;
    std::filesystem::create_directories(fixture.workspace / "nested");

    const auto traversal = evaluate_filesystem_request("read",
        R"({"path":"nested/../inside.txt"})",
        fixture.context(Session::Mode::PLAN));
    CHECK(traversal.decision.kind == PermissionDecision::Kind::ACCEPT);
    REQUIRE(traversal.request.has_value());
    CHECK(traversal.request->target == fixture.workspace / "inside.txt");

    const auto missing = evaluate_filesystem_request("write",
        write_args(fixture.workspace / "nested" / "new.txt"),
        fixture.context(Session::Mode::BUILD));
    CHECK(missing.decision.kind == PermissionDecision::Kind::ACCEPT);
    REQUIRE(missing.request.has_value());
    CHECK(missing.request->target == fixture.workspace / "nested" / "new.txt");

    const auto missing_parent = evaluate_filesystem_request("write",
        write_args(fixture.workspace / "absent" / "new.txt"),
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
    const auto linked_read = evaluate_filesystem_request("read",
        path_args("path", fixture.workspace / "outside-link" / "outside.txt"),
        fixture.context(Session::Mode::PLAN));
    CHECK(linked_read.decision.kind == PermissionDecision::Kind::ASK);
    REQUIRE(linked_read.request.has_value());
    CHECK(linked_read.request->target == fixture.outside / "outside.txt");

    const auto linked_write = evaluate_filesystem_request("write",
        write_args(fixture.workspace / "outside-link" / "new.txt"),
        fixture.context(Session::Mode::BUILD));
    CHECK(linked_write.decision.kind == PermissionDecision::Kind::ASK);
    REQUIRE(linked_write.request.has_value());
    CHECK(linked_write.request->target == fixture.outside / "new.txt");
#endif
}

TEST_CASE("filesystem policy rejects malformed operation arguments")
{
    PermissionFixture fixture;
    const PermissionContext context = fixture.context(Session::Mode::BUILD);
    const std::string file = (fixture.workspace / "inside.txt").string();

    CHECK(evaluate_filesystem_request(
              "read", R"({"path":")" + file + R"(","line_begin":0})", context)
              .decision.kind
        == PermissionDecision::Kind::REJECT);
    CHECK(evaluate_filesystem_request("read",
              R"({"path":")" + file + R"(","line_begin":4,"line_end":2})",
              context)
              .decision.kind
        == PermissionDecision::Kind::REJECT);
    CHECK(evaluate_filesystem_request("edit",
              R"({"file_path":")" + file
                  + R"(","old_string":"","new_string":"x"})",
              context)
              .decision.kind
        == PermissionDecision::Kind::REJECT);
    CHECK(evaluate_filesystem_request("write",
              R"({"file_path":")" + file + R"(","text":"x","overwrite":true})",
              context)
              .decision.kind
        == PermissionDecision::Kind::REJECT);
    CHECK(evaluate_filesystem_request("write",
              R"({"file_path":")" + file + R"(","text":"x","line":"one"})",
              context)
              .decision.kind
        == PermissionDecision::Kind::REJECT);
}

} // namespace ursa
