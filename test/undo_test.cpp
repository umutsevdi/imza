#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/application_state.h"
#include "app/flows.h"
#include "app/slash_commands.h"
#include "common/tool_call.h"
#include "conversation/session.h"
#include "network/json.h"
#include "test_fs.h"
#include "test_helpers.h"
#include "test_state.h"
#include "ui/ui.h"

namespace fs = std::filesystem;

namespace {

// A BUILD-mode host over `dir`, so the lua bindings record changes exactly
// as the application does.
imza::LuaHost workspace_host(imza::test::TempDir& dir)
{
    auto system    = std::make_shared<imza::SystemEnvironment>();
    auto workspace = std::make_shared<imza::WorkspaceEnvironment>();
    workspace->working_directory = dir.path;
    workspace->project_root      = dir.path;
    auto store                   = std::make_shared<imza::PermissionStore>();
    imza::LuaHost host;
    host.permission_context = [system, workspace, store] {
        return imza::PermissionContext { system, workspace, store->snapshot(),
            imza::SessionMode::BUILD };
    };
    // Removal always asks; approve it so a script can exercise the full
    // mutation set.
    host.ask = [](imza::ModalPayload) {
        std::promise<imza::ModalResult> promise;
        promise.set_value(
            imza::ToolVerdict { imza::ToolDecision::ACCEPT_ONCE, "" });
        return promise.get_future();
    };
    return host;
}

std::string lua_string(const std::string& value) { return "[[" + value + "]]"; }

// A Build-mode state rooted in a fresh temp directory, with the
// environment scan finished so the permission context is live.
struct UndoEnv {
    imza::test::TempDir dir;
    imza::test::CurrentDirectory cwd;
    std::shared_ptr<imza::ApplicationState> state;

    UndoEnv()
        : cwd()
    {
        fs::current_path(dir.path);
        state = imza::test::make_test_state();
        imza::test::wait_for_environment(*state);
        state->session->set_mode(imza::Session::Mode::BUILD);
    }

    std::string path(const std::string& name) const
    {
        return (dir.path / name).string();
    }

    void write(const std::string& name, const std::string& content) const
    {
        imza::test::write_file(dir.path / name, content);
    }

    std::string read(const std::string& name) const
    {
        return imza::test::read_all(dir.path / name);
    }

    // Records changes the way the turn runner does: a tool call in the
    // transcript, then its result.
    void record(std::vector<imza::FileChange> changes)
    {
        const imza::ToolCallRequest request { "lua", "{}", "", "call-1" };
        imza::test::append_tool(*state->session, request);
        imza::ToolCall::Result result { imza::ToolCall::Result::Kind::OUTPUT,
            "ok" };
        result.changes = std::move(changes);
        state->session->fill_tool_result(request, std::move(result));
    }

    imza::FileChange change(const std::string& name, std::string before,
        std::string after,
        imza::FileChange::Kind kind = imza::FileChange::Kind::MODIFIED) const
    {
        return { path(name), std::move(before), std::move(after), kind };
    }
};

imza::FileChange directory_change(
    const std::string& path, imza::FileChange::Kind kind)
{
    imza::FileChange change { path, "", "", kind };
    change.directory = true;
    return change;
}

} // namespace

TEST_CASE("each accepted mutation records one change per call")
{
    imza::test::TempDir dir;
    imza::test::write_file(dir.file("a.txt"), "one\ntwo\n");
    const std::string a = dir.file("a.txt").string();

    const imza::ToolOutput out
        = imza::test::run_lua("imza.fs.edit(" + lua_string(a)
                + ", 'one', 'ONE')\n"
                  "imza.fs.edit("
                + lua_string(a) + ", 'two', 'TWO')",
            workspace_host(dir));

    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    // The diff collapses; the undo records do not.
    REQUIRE(out.diffs.size() == 1);
    REQUIRE(out.changes.size() == 2);
    CHECK(out.changes[0].before == "one\ntwo\n");
    CHECK(out.changes[0].after == "ONE\ntwo\n");
    CHECK(out.changes[1].before == "ONE\ntwo\n");
    CHECK(out.changes[1].after == "ONE\nTWO\n");
    for (const imza::FileChange& change : out.changes) {
        CHECK(change.kind == imza::FileChange::Kind::MODIFIED);
        CHECK(change.path == a);
    }
}

TEST_CASE("changes record their kind")
{
    imza::test::TempDir dir;
    imza::test::write_file(dir.file("gone.txt"), "bye\n");
    imza::test::write_file(dir.file("source.txt"), "payload\n");
    const std::string fresh  = dir.file("fresh.txt").string();
    const std::string gone   = dir.file("gone.txt").string();
    const std::string source = dir.file("source.txt").string();
    const std::string moved  = dir.file("moved.txt").string();

    const imza::ToolOutput out
        = imza::test::run_lua("imza.fs.write(" + lua_string(fresh)
                + ", 'new')\n"
                  "imza.fs.remove("
                + lua_string(gone)
                + ")\n"
                  "imza.fs.move("
                + lua_string(source) + ", " + lua_string(moved) + ")",
            workspace_host(dir));

    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    REQUIRE(out.changes.size() == 4);
    CHECK(out.changes[0].kind == imza::FileChange::Kind::ADDED);
    CHECK(out.changes[0].before.empty());
    CHECK(out.changes[0].after == "new");
    CHECK(out.changes[1].kind == imza::FileChange::Kind::REMOVED);
    CHECK(out.changes[1].before == "bye\n");
    CHECK(out.changes[1].after.empty());
    // A move records both sides, destination first.
    CHECK(out.changes[2].kind == imza::FileChange::Kind::ADDED);
    CHECK(out.changes[2].path == moved);
    CHECK(out.changes[3].kind == imza::FileChange::Kind::REMOVED);
    CHECK(out.changes[3].path == source);
    CHECK(out.changes[3].before == "payload\n");
}

TEST_CASE("existence decides whether a write is a creation")
{
    imza::test::TempDir dir;
    // Empty and missing both read as ""; only the missing one is a
    // creation, and misclassifying it makes /undo delete a real file.
    imza::test::write_file(dir.file("empty.txt"), "");
    imza::test::write_file(dir.file("source.txt"), "new\n");
    imza::test::write_file(dir.file("target.txt"), "old\n");
    // A binary target cannot be read back, so its overwrite cannot be
    // inverted; it must still not count as a creation.
    imza::test::write_file(dir.file("target.bin"), std::string("a\0b", 3));

    const imza::ToolOutput out = imza::test::run_lua("imza.fs.write("
            + lua_string(dir.file("empty.txt").string())
            + ", 'filled')\n"
              "imza.fs.copy("
            + lua_string(dir.file("source.txt").string()) + ", "
            + lua_string(dir.file("target.txt").string())
            + ")\n"
              "imza.fs.copy("
            + lua_string(dir.file("source.txt").string()) + ", "
            + lua_string(dir.file("target.bin").string()) + ")",
        workspace_host(dir));

    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    REQUIRE(out.changes.size() == 2);
    CHECK(out.changes[0].kind == imza::FileChange::Kind::MODIFIED);
    CHECK(out.changes[0].before.empty());
    CHECK(out.changes[0].after == "filled");
    // Overwriting keeps the old bytes for the inverse.
    CHECK(out.changes[1].kind == imza::FileChange::Kind::MODIFIED);
    CHECK(out.changes[1].before == "old\n");
    CHECK(out.changes[1].after == "new\n");
}

TEST_CASE("mutations that change nothing record no step")
{
    imza::test::TempDir dir;
    imza::test::write_file(dir.file("a.txt"), "same\n");
    fs::create_directory(dir.file("there"));

    // Neither would change anything, and both would waste an /undo press.
    const imza::ToolOutput out = imza::test::run_lua("imza.fs.edit("
            + lua_string(dir.file("a.txt").string())
            + ", 'same', 'same')\n"
              "imza.fs.mkdir("
            + lua_string(dir.file("there").string()) + ")",
        workspace_host(dir));

    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.changes.empty());
}

TEST_CASE("a shell command marks the run's changes")
{
    imza::test::TempDir dir;
    const std::string path = dir.file("a.txt").string();
    imza::LuaHost host     = workspace_host(dir);
    host.shell_enabled     = true;

    const imza::ToolOutput with_shell = imza::test::run_lua(
        "imza.fs.write(" + lua_string(path) + ", 'x')\nimza.shell('true')",
        host);
    REQUIRE(with_shell.changes.size() == 1);
    CHECK(with_shell.changes[0].shell_side_effects);

    const imza::ToolOutput without = imza::test::run_lua(
        "imza.fs.write(" + lua_string(path) + ", 'y')", workspace_host(dir));
    REQUIRE(without.changes.size() == 1);
    CHECK_FALSE(without.changes[0].shell_side_effects);
}

TEST_CASE("the undo stack pops newest first")
{
    imza::Session session;
    session.push_undo({ { "a", "1", "2", imza::FileChange::Kind::MODIFIED },
        { "b", "3", "4", imza::FileChange::Kind::MODIFIED } });
    session.push_undo({ { "c", "", "5", imza::FileChange::Kind::ADDED } });

    REQUIRE(session.undo_depth() == 3);
    CHECK(session.peek_undo()->path == "c");
    CHECK(session.pop_undo()->path == "c");
    CHECK(session.pop_undo()->path == "b");
    CHECK(session.peek_undo()->path == "a");
    CHECK(session.pop_undo()->path == "a");
    CHECK_FALSE(session.pop_undo().has_value());
    CHECK(session.undo_depth() == 0);
}

TEST_CASE("the undo stack is bounded")
{
    imza::Session session;
    for (std::size_t index = 0; index < imza::MAX_UNDO_STEPS + 10; ++index) {
        session.push_undo({ { "file-" + std::to_string(index), "", "x",
            imza::FileChange::Kind::ADDED } });
    }
    CHECK(session.undo_depth() == imza::MAX_UNDO_STEPS);
    CHECK(session.peek_undo()->path
        == "file-" + std::to_string(imza::MAX_UNDO_STEPS + 9));
    std::optional<imza::FileChange> oldest;
    for (auto change = session.pop_undo(); change.has_value();
        change       = session.pop_undo()) {
        oldest = change;
    }
    REQUIRE(oldest.has_value());
    CHECK(oldest->path == "file-10");

    // An entry over the byte cap is still kept: dropping the newest
    // change would make it un-undoable.
    imza::Session single;
    single.push_undo({ { "huge", std::string(imza::MAX_UNDO_BYTES + 1, 'x'), "",
        imza::FileChange::Kind::REMOVED } });
    CHECK(single.undo_depth() == 1);
    CHECK(single.peek_undo()->path == "huge");
}

TEST_CASE("switching conversations clears the undo stack")
{
    imza::Session session;
    session.push_undo({ { "a", "1", "2", imza::FileChange::Kind::MODIFIED } });
    session.set_pending_undo(
        imza::FileChange { "b", "1", "2", imza::FileChange::Kind::MODIFIED });
    REQUIRE(session.undo_depth() == 1);

    session.restore(imza::SessionSnapshot { });

    CHECK(session.undo_depth() == 0);
    CHECK_FALSE(session.pending_undo().has_value());
}

TEST_CASE("undo reverts a modification")
{
    imza::test::IsolatedDataHome home;
    UndoEnv env;
    env.write("a.txt", "after\n");
    // An empty "before" is a modification too: the file existed, so undo
    // restores it empty rather than deleting it.
    env.write("empty.txt", "filled");
    env.record({ env.change("a.txt", "before\n", "after\n"),
        env.change("empty.txt", "", "filled") });

    imza::undo_last(*env.state);
    CHECK(env.state->session->error().empty());
    CHECK(env.read("empty.txt").empty());
    CHECK(fs::exists(env.path("empty.txt")));

    imza::undo_last(*env.state);
    CHECK(env.state->session->error().empty());
    CHECK(env.read("a.txt") == "before\n");
    CHECK(env.state->session->undo_depth() == 0);
}

TEST_CASE("undo restores a deleted file")
{
    imza::test::IsolatedDataHome home;
    UndoEnv env;
    env.record({ env.change(
        "gone.txt", "content\n", "", imza::FileChange::Kind::REMOVED) });
    REQUIRE_FALSE(fs::exists(env.path("gone.txt")));

    imza::undo_last(*env.state);

    CHECK(env.state->session->error().empty());
    CHECK(env.read("gone.txt") == "content\n");
}

TEST_CASE("undo walks back one file change at a time")
{
    imza::test::IsolatedDataHome home;
    UndoEnv env;
    env.write("a.txt", "ONE\nTWO\n");
    env.record({ env.change("a.txt", "one\ntwo\n", "ONE\ntwo\n"),
        env.change("a.txt", "ONE\ntwo\n", "ONE\nTWO\n") });

    imza::undo_last(*env.state);
    CHECK(env.read("a.txt") == "ONE\ntwo\n");
    CHECK(env.state->session->undo_depth() == 1);

    imza::undo_last(*env.state);
    CHECK(env.read("a.txt") == "one\ntwo\n");
    CHECK(env.state->session->undo_depth() == 0);
}

TEST_CASE("a move is undone in reverse order")
{
    imza::test::IsolatedDataHome home;
    UndoEnv env;
    env.write("moved.txt", "payload\n");
    env.record({ env.change("moved.txt", "", "payload\n",
                     imza::FileChange::Kind::ADDED),
        env.change(
            "source.txt", "payload\n", "", imza::FileChange::Kind::REMOVED) });

    // The source-removal was recorded last, so it pops first.
    imza::undo_last(*env.state);
    CHECK(env.state->session->error().empty());
    CHECK(env.read("source.txt") == "payload\n");
    CHECK(fs::exists(env.path("moved.txt")));

    imza::undo_last(*env.state);
    REQUIRE(env.state->session->pending_undo().has_value());
    imza::resolve_modal(
        *env.state, imza::ToolVerdict { imza::ToolDecision::ACCEPT_ONCE, "" });
    CHECK_FALSE(fs::exists(env.path("moved.txt")));
    CHECK(env.state->session->undo_depth() == 0);
}

TEST_CASE("undo recreates a removed directory and removes a created one")
{
    imza::test::IsolatedDataHome home;
    UndoEnv env;
    env.record({ directory_change(
                     env.path("made"), imza::FileChange::Kind::ADDED),
        directory_change(env.path("gone"), imza::FileChange::Kind::REMOVED) });
    fs::create_directory(env.path("made"));

    imza::undo_last(*env.state);
    CHECK(env.state->session->error().empty());
    CHECK(fs::is_directory(env.path("gone")));

    imza::undo_last(*env.state);
    REQUIRE(env.state->session->pending_undo().has_value());
    imza::resolve_modal(
        *env.state, imza::ToolVerdict { imza::ToolDecision::ACCEPT_ONCE, "" });
    CHECK_FALSE(fs::exists(env.path("made")));
}

TEST_CASE("undo refuses unsafe directory undos")
{
    imza::test::IsolatedDataHome home;
    UndoEnv env;

    env.record(
        { directory_change(env.path("made"), imza::FileChange::Kind::ADDED) });
    fs::create_directory(env.path("made"));
    env.write("made/later.txt", "written after the agent\n");
    imza::undo_last(*env.state);
    CHECK(env.state->session->error().find("contains files")
        != std::string::npos);
    CHECK(fs::exists(env.path("made/later.txt")));
    CHECK(env.state->session->undo_depth() == 1);

    imza::Session& session = *env.state->session;
    session.clear_undo();
    env.record({ directory_change(
        env.path("absent"), imza::FileChange::Kind::ADDED) });
    imza::undo_last(*env.state);
    CHECK(env.state->session->error().find("already deleted")
        != std::string::npos);

    session.clear_undo();
    env.record({ directory_change(
        env.path("taken"), imza::FileChange::Kind::REMOVED) });
    env.write("taken", "a file took the name\n");
    imza::undo_last(*env.state);
    CHECK(env.state->session->error().find("file now occupies")
        != std::string::npos);
    CHECK(env.read("taken") == "a file took the name\n");
}

TEST_CASE("undo deletes a created file after approval")
{
    imza::test::IsolatedDataHome home;
    UndoEnv env;
    env.write("fresh.txt", "new\n");
    env.record({ env.change(
        "fresh.txt", "", "new\n", imza::FileChange::Kind::ADDED) });

    imza::undo_last(*env.state);

    // Removing always asks, inside the workspace included.
    const imza::ModalPayload modal = env.state->session->modal();
    const auto* prompt = std::get_if<imza::PermissionPrompt>(&modal);
    REQUIRE(prompt != nullptr);
    CHECK(prompt->name == "remove");
    CHECK(prompt->id == "undo");
    CHECK(prompt->description.find("Revert") != std::string::npos);
    // The entry is parked, not lost, while the prompt is open.
    CHECK(env.state->session->undo_depth() == 0);
    CHECK(env.state->session->pending_undo().has_value());
    CHECK(fs::exists(env.path("fresh.txt")));

    imza::resolve_modal(
        *env.state, imza::ToolVerdict { imza::ToolDecision::ACCEPT_ONCE, "" });

    CHECK(env.state->session->error().empty());
    CHECK_FALSE(fs::exists(env.path("fresh.txt")));
    CHECK_FALSE(env.state->session->pending_undo().has_value());
}

TEST_CASE("a declined undo prompt restores the entry")
{
    imza::test::IsolatedDataHome home;
    UndoEnv env;
    env.write("fresh.txt", "new\n");
    env.record({ env.change(
        "fresh.txt", "", "new\n", imza::FileChange::Kind::ADDED) });

    imza::undo_last(*env.state);
    REQUIRE(env.state->session->pending_undo().has_value());
    imza::resolve_modal(*env.state,
        imza::ToolVerdict { imza::ToolDecision::REJECT, "no thanks" });
    CHECK(fs::exists(env.path("fresh.txt")));
    CHECK(env.state->session->undo_depth() == 1);
    CHECK(env.state->session->peek_undo()->path == env.path("fresh.txt"));

    imza::undo_last(*env.state);
    REQUIRE(env.state->session->pending_undo().has_value());
    imza::resolve_modal(*env.state, std::monostate { });
    CHECK(fs::exists(env.path("fresh.txt")));
    CHECK(env.state->session->undo_depth() == 1);
}

TEST_CASE("undo refuses an out-of-workspace target without approval")
{
    imza::test::IsolatedDataHome home;
    UndoEnv env;
    // Outside the workspace root, so the write asks.
    imza::test::TempDir outside;
    imza::test::write_file(outside.file("a.txt"), "after\n");
    env.record({ { outside.file("a.txt").string(), "before\n", "after\n",
        imza::FileChange::Kind::MODIFIED } });

    imza::undo_last(*env.state);

    REQUIRE(std::holds_alternative<imza::PermissionPrompt>(
        env.state->session->modal()));
    CHECK(imza::test::read_all(outside.file("a.txt")) == "after\n");

    imza::resolve_modal(*env.state,
        imza::ToolVerdict { imza::ToolDecision::ACCEPT_FOR_SESSION, "" });

    CHECK(env.state->session->error().empty());
    CHECK(imza::test::read_all(outside.file("a.txt")) == "before\n");
    CHECK_FALSE(env.state->permissions->snapshot()->empty());
}

TEST_CASE("undo refuses when the target drifted")
{
    imza::test::IsolatedDataHome home;
    UndoEnv env;

    env.write("a.txt", "user edit\n");
    env.record({ env.change("a.txt", "before\n", "agent\n") });
    imza::undo_last(*env.state);
    CHECK(
        env.state->session->error().find("changed since") != std::string::npos);
    CHECK(env.read("a.txt") == "user edit\n");
    CHECK(env.state->session->undo_depth() == 1);

    env.state->session->clear_undo();
    env.record({ env.change("missing.txt", "before\n", "after\n") });
    imza::undo_last(*env.state);
    CHECK(env.state->session->error().find("deleted") != std::string::npos);

    env.state->session->clear_undo();
    env.record({ env.change(
        "back.txt", "original\n", "", imza::FileChange::Kind::REMOVED) });
    env.write("back.txt", "recreated\n");
    imza::undo_last(*env.state);
    CHECK(env.state->session->error().find("recreated") != std::string::npos);
    CHECK(env.read("back.txt") == "recreated\n");
}

TEST_CASE("undo refuses when it cannot proceed")
{
    imza::test::IsolatedDataHome home;
    UndoEnv env;

    imza::undo_last(*env.state);
    CHECK(env.state->session->error() == "Nothing to undo.");

    env.write("a.txt", "after\n");
    env.record({ env.change("a.txt", "before\n", "after\n") });
    env.state->session->set_phase(imza::Session::Phase::STREAMING);
    imza::undo_last(*env.state);
    CHECK(
        env.state->session->error().find("pending work") != std::string::npos);
    CHECK(env.state->session->undo_depth() == 1);
    env.state->session->set_phase(imza::Session::Phase::IDLE);

    env.state->session->set_mode(imza::Session::Mode::PLAN);
    imza::undo_last(*env.state);
    CHECK(env.state->session->error().find("Plan mode") != std::string::npos);
    CHECK(env.state->session->undo_depth() == 1);
    env.state->session->set_mode(imza::Session::Mode::BUILD);

    // An open prompt must be resolved first, and the older entry stays.
    env.record(
        { env.change("b.txt", "", "new\n", imza::FileChange::Kind::ADDED) });
    env.write("b.txt", "new\n");
    imza::undo_last(*env.state);
    REQUIRE(env.state->session->pending_undo().has_value());
    imza::undo_last(*env.state);
    CHECK(
        env.state->session->error().find("pending undo") != std::string::npos);
    CHECK(env.state->session->undo_depth() == 1);
}

TEST_CASE("a prompt outside a turn leaves the session idle")
{
    imza::test::IsolatedDataHome home;
    UndoEnv env;
    env.write("fresh.txt", "new\n");
    env.record({ env.change(
        "fresh.txt", "", "new\n", imza::FileChange::Kind::ADDED) });
    REQUIRE(env.state->session->phase() == imza::Session::Phase::IDLE);

    // Regression: the prompt used to park the session in AWAITING, which
    // resolve_modal turned into CONNECTING - a phase only a turn advances.
    imza::undo_last(*env.state);
    CHECK(env.state->session->phase() == imza::Session::Phase::IDLE);
    imza::resolve_modal(
        *env.state, imza::ToolVerdict { imza::ToolDecision::ACCEPT_ONCE, "" });
    CHECK(env.state->session->phase() == imza::Session::Phase::IDLE);
    CHECK_FALSE(env.state->session->has_pending_work());

    // A rejected skill prompt took the same path.
    imza::PermissionPrompt skill;
    skill.name    = "skill";
    skill.id      = "manual-skill";
    skill.request = imza::SkillRequest { "docs", "project", "/tmp/SKILL.md" };
    imza::enqueue_user_modal(*env.state, skill);
    REQUIRE(std::holds_alternative<imza::PermissionPrompt>(
        env.state->session->modal()));
    CHECK(env.state->session->phase() == imza::Session::Phase::IDLE);
    imza::resolve_modal(
        *env.state, imza::ToolVerdict { imza::ToolDecision::REJECT, "no" });
    CHECK(env.state->session->phase() == imza::Session::Phase::IDLE);

    // A turn blocked on an approval is still AWAITING.
    env.state->session->set_phase(imza::Session::Phase::STREAMING);
    env.state->session->present_modal(imza::PermissionPrompt { });
    CHECK(env.state->session->phase() == imza::Session::Phase::AWAITING);
}

TEST_CASE("a build subagent's edits reach the parent undo stack")
{
    imza::test::IsolatedDataHome home;
    imza::test::CurrentDirectory cwd;
    imza::test::TempDir dir;
    fs::current_path(dir.path);
    imza::test::PostPump pump;
    auto state = imza::test::make_test_state(pump.fn(),
        imza::test::test_config(),
        [&dir](const imza::ChatRequest& req,
            const imza::StreamCallback& cb) -> imza::Status {
            const bool child           = std::any_of(req.messages.begin(),
                req.messages.end(), [](const imza::Message& message) {
                    return message.type == imza::Message::Type::USER
                        && message.content.starts_with("write a file");
                });
            const bool has_tool_result = std::any_of(req.messages.begin(),
                req.messages.end(), [](const imza::Message& message) {
                    return message.type == imza::Message::Type::TOOL;
                });
            if (!child && !has_tool_result) {
                cb(imza::make_tool_call_event({ "subagent",
                    R"({"tasks":[{"mode":"build","prompt":"write a file"}]})",
                    "delegate", "delegate-1" }));
            } else if (child && !has_tool_result) {
                imza::JsonValue args = imza::JsonValue::object_t { };
                args.get<imza::JsonValue::object_t>()["script"]
                    = imza::JsonValue("imza.fs.write([["
                        + (dir.path / "child.txt").string()
                        + "]], 'child wrote this')");
                cb(imza::make_tool_call_event(
                    { "lua", imza::json_dump(args), "write", "child-lua" }));
            } else {
                cb(imza::make_delta_event("done"));
            }
            cb(imza::make_done_event());
            return imza::Status::OK;
        });
    REQUIRE(pump.wait_for([&] { return state->environment->ready(); }));
    state->session->set_mode(imza::Session::Mode::BUILD);

    imza::submit(*state, "delegate a build");
    REQUIRE(pump.wait_for([&] { return imza::test::idle(*state->session); }));
    REQUIRE(state->session->undo_depth() > 0);

    // The child owned its own conversation, so its record had to be
    // folded into the parent's stack.
    const std::optional<imza::FileChange> change = state->session->peek_undo();
    REQUIRE(change.has_value());
    CHECK(change->path == (dir.path / "child.txt").string());
    CHECK(change->after == "child wrote this");
    CHECK(imza::test::read_all(dir.path / "child.txt") == "child wrote this");

    imza::undo_last(*state);
    REQUIRE(state->session->pending_undo().has_value());
    imza::resolve_modal(
        *state, imza::ToolVerdict { imza::ToolDecision::ACCEPT_ONCE, "" });
    CHECK(state->session->error().empty());
    CHECK_FALSE(fs::exists(dir.path / "child.txt"));
    CHECK(state->session->undo_depth() == 0);
}

TEST_CASE("/undo routes through the slash command")
{
    const imza::SlashCommand* command = imza::find_command("/undo");
    REQUIRE(command != nullptr);
    CHECK(command->action == imza::SlashCommand::Action::UNDO);
    CHECK_FALSE(command->takes_argument);
    CHECK(imza::find_command("/revert") == command);

    imza::test::IsolatedDataHome home;
    UndoEnv env;
    env.write("a.txt", "after\n");
    env.record({ env.change("a.txt", "before\n", "after\n") });

    imza::run_slash(*env.state, "/undo");

    CHECK(env.state->session->error().empty());
    CHECK(env.read("a.txt") == "before\n");
}

TEST_CASE("a new session clears the undo stack")
{
    imza::test::IsolatedDataHome home;
    UndoEnv env;
    env.write("a.txt", "after\n");
    env.record({ env.change("a.txt", "before\n", "after\n") });
    env.record({ });
    REQUIRE(env.state->session->undo_depth() == 1);

    imza::run_slash(*env.state, "/new");

    CHECK(env.state->session->undo_depth() == 0);
    CHECK(env.read("a.txt") == "after\n");
}

TEST_CASE("the agent learns about the undo")
{
    imza::test::IsolatedDataHome home;
    UndoEnv env;
    env.write("a.txt", "after\n");
    env.record({ env.change("a.txt", "before\n", "after\n") });

    imza::undo_last(*env.state);
    REQUIRE(env.state->session->error().empty());

    // The notice shows in the timeline...
    const auto items   = env.state->session->items();
    const auto* notice = std::get_if<imza::Notice>(&items->back());
    REQUIRE(notice != nullptr);
    CHECK(notice->text.find("reverted") != std::string::npos);
    CHECK(notice->text.find(env.path("a.txt")) != std::string::npos);

    // ...and replays into model history as a tagged user message.
    const auto history = env.state->session->build_history(
        "system", imza::ApiStandard::OPENAI);
    const auto found = std::find_if(
        history.begin(), history.end(), [](const imza::Message& message) {
            return message.content.find("<notice>") != std::string::npos
                && message.content.find("reverted") != std::string::npos;
        });
    REQUIRE(found != history.end());
    CHECK(found->type == imza::Message::Type::USER);
    CHECK(found->content.find(env.path("a.txt")) != std::string::npos);
}

TEST_CASE("the chat hint shows undo availability")
{
    imza::test::IsolatedDataHome home;
    auto state = imza::test::make_test_state();
    state->session->begin_send("hello");
    auto chat
        = imza::make_chat(state, [] { return imza::test::wide_layout(); });
    const auto render = [&chat] {
        return imza::test::without_ansi(
            imza::test::to_text(chat->Render(), 100, 40));
    };

    CHECK(render().find("/undo") == std::string::npos);

    state->session->push_undo(
        { { "a", "1", "2", imza::FileChange::Kind::MODIFIED } });
    const std::string one = render();
    CHECK(one.find("/undo") != std::string::npos);
    CHECK(one.find("1") != std::string::npos);

    // A second change updates the hint instead of leaving it stale.
    state->session->push_undo(
        { { "b", "1", "2", imza::FileChange::Kind::MODIFIED } });
    const std::string two = render();
    CHECK(two.find("/undo") != std::string::npos);
    CHECK(two.find("2") != std::string::npos);
    CHECK(two != one);
}

TEST_CASE("a failed apply puts the entry back")
{
    imza::test::IsolatedDataHome home;
    UndoEnv env;
    env.write("nested/a.txt", "after\n");
    env.record({ env.change("nested/a.txt", "before\n", "after\n") });

    fs::remove_all(env.path("nested"));
    env.write("nested", "not a directory\n");

    imza::undo_last(*env.state);

    CHECK_FALSE(env.state->session->error().empty());
    CHECK(env.state->session->undo_depth() == 1);
}
