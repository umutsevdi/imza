#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include "common/util.h"
#include "conversation/session.h"
#include "permissions/store.h"
#include "test_fs.h"
#include "test_helpers.h"
#include "tools/tool.h"
#include "workspace/environment.h"

namespace fs = std::filesystem;

namespace {

// A lua host wired to a session's plan document.
imza::LuaHost plan_session_host(imza::Session& session)
{
    imza::LuaHost host { };
    host.plan_doc    = [&] { return session.plan_doc(); };
    host.create_plan = [&](std::string content) {
        return session.create_plan(std::move(content));
    };
    host.edit_plan = [&](const std::string& old, const std::string& fresh,
                         std::size_t count) {
        return session.edit_plan(old, fresh, count);
    };
    host.mark_plan_seen = [&] { session.mark_plan_seen(); };
    host.plan_frozen    = [] { return false; };
    return host;
}

// A host wired like the application: a real permission context built from
// the live permission store, an attended ask route, and grant installation
// back into the store. Tests pin behavior without spinning up a full state.
struct ShellFixture {
    std::shared_ptr<imza::SystemEnvironment> system
        = std::make_shared<imza::SystemEnvironment>();
    std::shared_ptr<imza::WorkspaceEnvironment> workspace
        = std::make_shared<imza::WorkspaceEnvironment>();
    imza::PermissionStore store;
    imza::PermissionStore::Grants installed;
    std::optional<imza::ToolVerdict> verdict;
    std::optional<imza::PermissionPrompt> last_prompt;
    int ask_calls         = 0;
    bool attendable       = true;
    bool shell_enabled    = true;
    bool skip_permissions = false;

    bool install(const imza::PermissionStore::Grants& grants)
    {
        return store.install(grants);
    }

    imza::LuaHost host()
    {
        imza::LuaHost host;
        host.shell_enabled      = shell_enabled;
        host.skip_permissions   = skip_permissions;
        host.permission_context = [this] {
            return imza::PermissionContext { system, workspace,
                store.snapshot(), imza::SessionMode::BUILD };
        };
        host.install_grants = [this](imza::PermissionStore::Grants grants) {
            installed.insert(installed.end(), grants.begin(), grants.end());
            return store.install(std::move(grants));
        };
        if (attendable) {
            host.ask = [this](imza::ModalPayload payload) {
                ++ask_calls;
                last_prompt
                    = std::get<imza::PermissionPrompt>(std::move(payload));
                std::promise<imza::ModalResult> promise;
                promise.set_value(verdict.has_value()
                        ? imza::ModalResult { *verdict }
                        : imza::ModalResult { imza::ToolVerdict {
                              imza::ToolDecision::REJECT, "test denial" } });
                return promise.get_future();
            };
        }
        return host;
    }
};

// A host whose permission context sees a BUILD-mode workspace rooted in
// `dir`, backed by a live (empty) permission store; tests attach an ask
// route on the returned host when they need one.
struct WorkspaceFixture {
    imza::test::TempDir dir;
    std::shared_ptr<imza::SystemEnvironment> system
        = std::make_shared<imza::SystemEnvironment>();
    std::shared_ptr<imza::WorkspaceEnvironment> workspace
        = std::make_shared<imza::WorkspaceEnvironment>();
    imza::PermissionStore store;

    WorkspaceFixture()
    {
        workspace->working_directory = dir.path;
        workspace->project_root      = dir.path;
    }

    imza::LuaHost host()
    {
        imza::LuaHost host;
        host.permission_context = [this] {
            return imza::PermissionContext { system, workspace,
                store.snapshot(), imza::SessionMode::BUILD };
        };
        return host;
    }
};

} // namespace

TEST_CASE("lua tool prints values to its output")
{
    const imza::ToolOutput out
        = imza::test::run_lua("print('hello', 42, true)");
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text == "hello    42    true\n");
}

TEST_CASE("binding failures raise and abort; pcall tolerates")
{
    // Every binding failure raises: an operational failure (missing file)
    // aborts the script just like a type mistake.
    const imza::ToolOutput raised = imza::test::run_lua(
        "imza.fs.read('no-such-file.txt')\nprint('dead')");
    CHECK(raised.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(raised.text.find("no such file") != std::string::npos);
    CHECK(raised.text.find("dead") == std::string::npos);

    // A wrong argument type is a script bug: it raises with a Lua error
    // position. (Numbers coerce to strings per the Lua C API; nil,
    // booleans, and tables are the rejected shapes.)
    const imza::ToolOutput bad_type
        = imza::test::run_lua("imza.fs.read({})\nprint('dead')");
    CHECK(bad_type.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(bad_type.text.find("bad argument #1 to 'read'") != std::string::npos);
    CHECK(
        bad_type.text.find("string expected, got table") != std::string::npos);

    // pcall is the sanctioned way to survive any expected failure.
    const imza::ToolOutput caught = imza::test::run_lua(
        "local ok, err = pcall(imza.fs.read, 'no-such-file.txt')\n"
        "print(ok, err)");
    CHECK(caught.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(caught.text.find("false") == std::size_t(0));
    CHECK(caught.text.find("no such file") != std::string::npos);
}

TEST_CASE("lua tool rejects empty scripts and reports errors with positions")
{
    CHECK(imza::test::run_lua("").kind == imza::ToolOutput::Kind::ERROR);

    const imza::ToolOutput runtime = imza::test::run_lua("error('boom')");
    CHECK(runtime.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(runtime.text.find("boom") != std::string::npos);

    const imza::ToolOutput syntax = imza::test::run_lua("retuern 1");
    CHECK(syntax.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(syntax.text.find(":1:") != std::string::npos);
}

TEST_CASE("lua sandbox denies io, os, and binary chunks")
{
    CHECK(imza::test::run_lua("io.read()")
              .text.find("attempt to index a nil value")
        != std::string::npos);

    CHECK(imza::test::run_lua("os.execute('touch /tmp/imza_pwn')").kind
        == imza::ToolOutput::Kind::ERROR);

    const imza::ToolOutput chunk
        = imza::test::run_lua("print(load('\\27Lua binary') == nil)");
    CHECK(chunk.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(chunk.text.find("true") != std::string::npos);
}

TEST_CASE("lua tool captures top-level return values as JSON")
{
    const imza::ToolOutput none = imza::test::run_lua("print('x')");
    CHECK(none.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(none.text == "x\n");
    CHECK_FALSE(none.return_value.has_value());

    const imza::ToolOutput object = imza::test::run_lua(
        "print('log')\n"
        "return { file = 'a.cpp', changed = true, lines = { 10, 11 } }");
    CHECK(object.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(object.text == "log\n");
    REQUIRE(object.return_value.has_value());
    const imza::JsonValue* file
        = imza::find_member(*object.return_value, "file");
    REQUIRE(file != nullptr);
    CHECK(file->as<std::string>() == "a.cpp");
    const imza::JsonValue* changed
        = imza::find_member(*object.return_value, "changed");
    REQUIRE(changed != nullptr);
    REQUIRE(changed->is_boolean());
    CHECK(changed->get<bool>());
    const imza::JsonValue* lines
        = imza::find_member(*object.return_value, "lines");
    REQUIRE(lines != nullptr);
    REQUIRE(lines->is_array());
    const auto& line_values = lines->get<imza::JsonValue::array_t>();
    CHECK(line_values.size() == 2);
    CHECK(line_values[1].as<int>() == 11);

    const imza::ToolOutput scalars = imza::test::run_lua("return 42");
    CHECK(scalars.return_value.has_value());
    CHECK(scalars.return_value->as<int>() == 42);

    const imza::ToolOutput null = imza::test::run_lua("return nil");
    CHECK(null.return_value.has_value());
    CHECK(null.return_value->is_null());

    const imza::ToolOutput multi = imza::test::run_lua("return 'a', 2, false");
    REQUIRE(multi.return_value.has_value());
    const imza::JsonValue& multi_value = *multi.return_value;
    REQUIRE(multi_value.is_array());
    const auto& multi_array = multi_value.get<imza::JsonValue::array_t>();
    CHECK(multi_array.size() == 3);
    CHECK(multi_array[0].as<std::string>() == "a");
    CHECK(multi_array[1].as<int>() == 2);
    REQUIRE(multi_array[2].is_boolean());
    CHECK_FALSE(multi_array[2].get<bool>());

    const imza::ToolOutput empty = imza::test::run_lua("return {}");
    REQUIRE(empty.return_value.has_value());
    CHECK(empty.return_value->is_object());
    CHECK(empty.return_value->get<imza::JsonValue::object_t>().empty());
}

TEST_CASE("lua tool rejects unconvertible return values")
{
    const auto rejected
        = [](const std::string& script, const std::string& reason) {
              const imza::ToolOutput out = imza::test::run_lua(script);
              CHECK(out.kind == imza::ToolOutput::Kind::ERROR);
              CHECK(out.text.find(reason) != std::string::npos);
              CHECK_FALSE(out.return_value.has_value());
          };

    rejected("return function() end", "unsupported function value");
    rejected("local t = {} t.self = t return t",
        "cyclic or repeated table reference");
    rejected(
        "local t = {} return { t, t }", "cyclic or repeated table reference");
    rejected(
        "return { 1, 2, [5] = 3 }", "array keys must be contiguous from 1");
    rejected("return { a = 1, [2] = 'b' }",
        "tables cannot mix array and object keys");
    rejected("return 0/0", "numbers must be finite");

    std::string deep = "return ";
    for (int i = 0; i < 17; ++i) {
        deep += "{ x = ";
    }
    deep += "1";
    for (int i = 0; i < 17; ++i) {
        deep += " }";
    }
    rejected(deep, "nesting exceeds 16");

    rejected("local t = {} for i = 1, 5001 do t[i] = i end return t",
        "table exceeds 5000 entries");

    rejected("return string.rep('x', 70000)", "encoded value exceeds 64 KiB");
}

TEST_CASE("return-value conversion failure preserves log and diffs")
{
    imza::test::TempDir dir;
    imza::test::write_file(dir.file("a.txt"), "one\n");
    const std::string script = "imza.fs.write('" + dir.file("a.txt").string()
        + "', 'two\\n')\n"
          "return function() end";
    const imza::ToolOutput out = imza::test::run_lua(script);
    CHECK(out.kind == imza::ToolOutput::Kind::ERROR);
    CHECK_FALSE(out.return_value.has_value());
    REQUIRE(out.dispatch_log.size() == 1);
    CHECK(out.dispatch_log[0].binding == "fs.write");
    REQUIRE(out.diffs.size() == 1);
    CHECK(out.diffs[0].file == dir.file("a.txt").string());
}

TEST_CASE("lua print truncation respects the hard output cap")
{
    const imza::ToolOutput out
        = imza::test::run_lua("for i = 1, 100000 do print(i, 'x', true) end");
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text.size() <= 64 * 1024);
    CHECK(out.text.find("[truncated]") != std::string::npos);
}

TEST_CASE("imza.fs.read returns 1-based windows and empty for empty files")
{
    imza::test::TempDir dir;
    imza::test::write_file(dir.file("lines.txt"), "alpha\nbeta\ngamma\n");
    imza::test::write_file(dir.file("empty.txt"), "");

    const imza::ToolOutput full = imza::test::run_lua(
        "return imza.fs.read([[" + dir.file("lines.txt").string() + "]])");
    REQUIRE(full.return_value.has_value());
    CHECK(full.return_value->as<std::string>() == "alpha\nbeta\ngamma\n");

    const imza::ToolOutput window = imza::test::run_lua("return imza.fs.read([["
        + dir.file("lines.txt").string() + "]], 2, 3)");
    REQUIRE(window.return_value.has_value());
    CHECK(window.return_value->as<std::string>() == "beta\ngamma\n");

    const imza::ToolOutput empty = imza::test::run_lua(
        "return imza.fs.read([[" + dir.file("empty.txt").string() + "]])");
    REQUIRE(empty.return_value.has_value());
    CHECK(empty.return_value->as<std::string>().empty());

    const imza::ToolOutput bad_range = imza::test::run_lua(
        "imza.fs.read([[" + dir.file("lines.txt").string() + "]], 0)");
    CHECK(bad_range.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(bad_range.text.find("1-based") != std::string::npos);
}

TEST_CASE("imza.fs.list returns entries with type and size fields")
{
    imza::test::TempDir dir;
    imza::test::write_file(dir.file("visible.txt"), "content");
    fs::create_directories(dir.file("sub"));
    imza::test::write_file(dir.file("sub/nested.txt"), "content");

    const imza::ToolOutput out
        = imza::test::run_lua("local rows = imza.fs.list([[" + dir.path.string()
            + "]], 2)\n"
              "for _, r in ipairs(rows) do print(r.path, r.type) end");
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text.find("visible.txt    file") != std::string::npos);
    CHECK(out.text.find("sub    dir") != std::string::npos);
    CHECK(out.text.find("sub/nested.txt    file") != std::string::npos);
}

TEST_CASE("imza.fs.grep returns file, line, and text per match")
{
    imza::test::TempDir dir;
    imza::test::write_file(
        dir.file("hay.cpp"), "int cat = 1;\nint dog = 2;\ncat();\n");

    const imza::ToolOutput out = imza::test::run_lua(
        "local rows = imza.fs.grep([[" + imza::utf8_from_path(dir.path)
        + "]], 'cat')\n"
          "print(#rows, rows[1].file, rows[1].line, rows[1].text)\n"
          "for _, r in ipairs(rows) do print(r.file, r.line, r.text) end");
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text.find("hay.cpp    1    int cat = 1;") != std::string::npos);
    CHECK(out.text.find("hay.cpp    3    cat();") != std::string::npos);
}

TEST_CASE("imza.fs.grep accepts a single file target and fills the file field")
{
    imza::test::TempDir dir;
    imza::test::write_file(dir.file("one.cpp"), "only match here\nnope\n");

    const imza::ToolOutput file_out
        = imza::test::run_lua("local rows = imza.fs.grep([["
            + imza::utf8_from_path(dir.file("one.cpp"))
            + "]], 'match')\n"
              "if #rows ~= 1 then error('expected 1 row') end\n"
              "print(rows[1].file, rows[1].line, rows[1].text)");
    CHECK(file_out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(file_out.text.find("one.cpp    1    only match here")
        != std::string::npos);
}

TEST_CASE("imza.fs.grep uses POSIX extended regular expressions")
{
    imza::test::TempDir dir;
    imza::test::write_file(dir.file("ere.txt"),
        "catcat 42\n"
        "dogcat 7\n"
        "cat 42\n"
        "catcat xx\n");

    const imza::ToolOutput out
        = imza::test::run_lua("local rows = imza.fs.grep([["
            + imza::utf8_from_path(dir.file("ere.txt"))
            + "]], '^(cat|dog){2}[[:space:]][[:digit:]]+$')\n"
              "for _, r in ipairs(rows) do print(r.line, r.text) end");
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text == "1    catcat 42\n2    dogcat 7\n");
}

#ifndef _WIN32
TEST_CASE("imza.fs.grep handles colons in filenames without parsing output")
{
    imza::test::TempDir dir;
    const fs::path file = dir.file("part:one.txt");
    imza::test::write_file(file, "needle: value\n");

    const imza::ToolOutput out = imza::test::run_lua(
        "local rows = imza.fs.grep([[" + imza::utf8_from_path(dir.path)
        + "]], 'needle')\n"
          "print(#rows, rows[1].file == [["
        + imza::utf8_from_path(file) + "]], rows[1].line, rows[1].text)");
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text == "1    true    1    needle: value\n");
}
#endif

TEST_CASE("imza.fs.grep handles UTF-8 filenames and content")
{
    imza::test::TempDir dir;
    const fs::path file = dir.file("café.txt");
    imza::test::write_file(file, "touché needle\n");
    const std::string file_name = imza::utf8_from_path(file);

    const imza::ToolOutput out
        = imza::test::run_lua("local rows = imza.fs.grep([[" + file_name
            + "]], 'needle')\n"
              "print(rows[1].file == [["
            + file_name + "]], rows[1].line, rows[1].text)");
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text == "true    1    touché needle\n");
}

TEST_CASE("imza.fs.grep normalizes CRLF lines")
{
    imza::test::TempDir dir;
    imza::test::write_file(
        dir.file("windows.txt"), "first\r\nmatching text\r\nlast");

    const imza::ToolOutput out
        = imza::test::run_lua("local rows = imza.fs.grep([["
            + imza::utf8_from_path(dir.file("windows.txt"))
            + "]], 'matching|last$')\n"
              "for _, r in ipairs(rows) do print(r.line, r.text) end");
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text == "2    matching text\n3    last\n");
}

TEST_CASE("imza.fs.grep raises on invalid expressions")
{
    imza::test::TempDir dir;
    imza::test::write_file(dir.file("text.txt"), "text\n");

    const imza::ToolOutput out = imza::test::run_lua("imza.fs.grep([["
        + imza::utf8_from_path(dir.path) + "]], '(')\nprint('dead')");
    CHECK(out.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(out.text.find("grep: invalid POSIX extended regular expression")
        != std::string::npos);
    CHECK(out.text.find("dead") == std::string::npos);
}

TEST_CASE("imza.fs.grep caps results and appends a truncation marker")
{
    imza::test::TempDir dir;
    std::string contents;
    for (int line = 0; line < 501; ++line) {
        contents += "match\n";
    }
    imza::test::write_file(dir.file("many.txt"), contents);

    const imza::ToolOutput out = imza::test::run_lua(
        "local rows = imza.fs.grep([[" + imza::utf8_from_path(dir.path)
        + "]], 'match')\n"
          "print(#rows, rows[500].line, rows[501].text)");
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text == "501    500    [truncated]\n");
}

TEST_CASE("imza.fs.grep skips binary files")
{
    imza::test::TempDir dir;
    imza::test::write_file(dir.file("text.txt"), "needle in text\n");
    imza::test::write_file(dir.file("binary.dat"),
        std::string("needle before null\0needle after null\n", 37));

    const imza::ToolOutput out = imza::test::run_lua(
        "local rows = imza.fs.grep([[" + imza::utf8_from_path(dir.path)
        + "]], 'needle')\n"
          "print(#rows, rows[1].text)");
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text == "1    needle in text\n");
}

TEST_CASE("bindings outside the workspace return an error value")
{
    imza::test::TempDir dir;
    // Without a provider the binding runs in trusted mode; with a provider
    // whose context lacks workspace/system it must reject, not crash.
    imza::LuaHost empty { };
    empty.permission_context   = [] { return imza::PermissionContext { }; };
    const imza::ToolOutput out = imza::test::run_lua(
        "imza.fs.read([[" + dir.file("lines.txt").string() + "]])",
        std::move(empty));
    CHECK(out.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(out.text.find("denied") != std::string::npos);
}

TEST_CASE("todo bindings read and write the shared task board")
{
    imza::LuaHost host { };
    imza::TodoList board;
    host.todo     = [&board] { return board; };
    host.set_todo = [&board](imza::TodoList todo) { board = std::move(todo); };

    const imza::ToolOutput set
        = imza::test::run_lua("imza.todo.set({"
                              "{content = 'first', status = 'in_progress'},"
                              "{content = 'second'},"
                              "{content = 'third', status = 'completed'},"
                              "{content = 'fourth', status = 'cancelled'}})",
            std::move(host));
    CHECK(set.kind == imza::ToolOutput::Kind::OUTPUT);
    REQUIRE(board.items.size() == 4);
    CHECK(board.items[0].content == "first");
    CHECK(board.items[0].status == imza::TodoItem::Status::IN_PROGRESS);
    CHECK(board.items[1].status == imza::TodoItem::Status::PENDING);
    CHECK(board.items[2].status == imza::TodoItem::Status::COMPLETED);
    CHECK(board.items[3].status == imza::TodoItem::Status::CANCELLED);

    imza::LuaHost host2 { };
    host2.todo     = [&board] { return board; };
    host2.set_todo = [&board](imza::TodoList todo) { board = std::move(todo); };
    const imza::ToolOutput get
        = imza::test::run_lua("local rows = imza.todo.get()\n"
                              "print(#rows, rows[1].content, rows[1].status, "
                              "rows[2].status, rows[3].status, rows[4].status)",
            std::move(host2));
    CHECK(get.text
        == "4    first    in_progress    pending    completed    cancelled\n");

    const imza::ToolOutput bad = imza::test::run_lua(
        "imza.todo.set({{content = 'x', status = 'nope'}})", imza::LuaHost { });
    CHECK(bad.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(bad.text.find("unknown status") != std::string::npos);
}

TEST_CASE("imza.ask surfaces answers and unattended runs reject")
{
    imza::LuaHost host { };
    host.ask
        = [](imza::ModalPayload payload) -> std::future<imza::ModalResult> {
        const auto& form = std::get<imza::QuestionForm>(payload);
        REQUIRE(form.size() == 1);
        CHECK(form[0].prompt == "deploy?");
        CHECK(form[0].options.size() == 2);
        imza::ModalAnswer answer;
        imza::QuestionAnswer qa;
        qa.prompt   = form[0].prompt;
        qa.selected = { "yes" };
        answer.cards.push_back(std::move(qa));
        std::promise<imza::ModalResult> promise;
        promise.set_value(std::move(answer));
        return promise.get_future();
    };

    const imza::ToolOutput out = imza::test::run_lua(
        "local rows = imza.ask({{prompt = 'deploy?', options = "
        "{'yes', 'no'}}})\n"
        "print(rows[1].question, rows[1].answer)",
        std::move(host));
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text == "deploy?    yes\n");

    const imza::ToolOutput unattended
        = imza::test::run_lua("imza.ask({{prompt = 'hello?'}})\nprint('dead')");
    CHECK(unattended.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(unattended.text.find("unavailable") != std::string::npos);
    CHECK(unattended.text.find("dead") == std::string::npos);
}

TEST_CASE("plan bindings validate skeleton and cap through the session")
{
    imza::Session session;
    imza::LuaHost host = plan_session_host(session);

    const std::string& skeleton = imza::test::PLAN_SKELETON;

    const imza::ToolOutput missing = imza::test::run_lua(
        "imza.plan.create('# Requirements only')\nprint('dead')", host);
    CHECK(missing.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(missing.text.find("missing required headings") != std::string::npos);
    CHECK(missing.text.find("approach") != std::string::npos);
    CHECK(missing.text.find("changes") != std::string::npos);
    CHECK(missing.text.find("dead") == std::string::npos);
    CHECK(session.plans().empty());

    std::string big = skeleton + "\n";
    big.resize(imza::MAX_PLAN_BYTES + 1, 'x');
    const imza::ToolOutput oversized = imza::test::run_lua(
        "imza.plan.create([[" + big + "]])\nprint('dead')", host);
    CHECK(oversized.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(oversized.text.find("cap") != std::string::npos);

    const imza::ToolOutput created
        = imza::test::run_lua("imza.plan.create([[" + skeleton
                + "]])\n"
                  "print(imza.plan.get():find('# Verification') ~= nil)",
            std::move(host));
    CHECK(created.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(created.text == "true\n");
    REQUIRE(session.plans().size() == 1);
    CHECK(session.plan_doc() == skeleton);
}

TEST_CASE("plan edit requires a prior read and patches the current document")
{
    imza::Session session;
    imza::LuaHost host = plan_session_host(session);

    const imza::ToolOutput no_plan
        = imza::test::run_lua("imza.plan.edit('a', 'b')\nprint('dead')", host);
    CHECK(no_plan.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(no_plan.text.find("no plan exists") != std::string::npos);

    const std::string& skeleton = imza::test::PLAN_SKELETON;
    REQUIRE(session.create_plan(skeleton).empty());

    const imza::ToolOutput reedited
        = imza::test::run_lua("imza.plan.get()\n"
                              "imza.plan.edit('x', 'settled')\n"
                              "print(imza.plan.get():find('settled') ~= nil)",
            std::move(host));
    CHECK(reedited.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(reedited.text == "true\n");
}

TEST_CASE("plan mutations are rejected while frozen in build mode")
{
    imza::LuaHost host { };
    host.plan_doc    = [] { return std::string { }; };
    host.create_plan = [](std::string) { return std::string { }; };
    host.edit_plan   = [](const std::string&, const std::string&, std::size_t) {
        return std::string { };
    };
    host.mark_plan_seen = [] { };
    host.plan_frozen    = [] { return true; };

    const imza::ToolOutput out = imza::test::run_lua(
        "imza.plan.create('# Requirements')\nprint('dead')", host);
    CHECK(out.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(out.text.find("plan.create: unavailable in Build mode")
        != std::string::npos);
    CHECK(out.text.find("dead") == std::string::npos);

    const imza::ToolOutput edit
        = imza::test::run_lua("imza.plan.edit('a', 'b')\nprint('dead')", host);
    CHECK(edit.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(edit.text.find("plan.edit: unavailable in Build mode")
        != std::string::npos);
}

TEST_CASE("plan bindings are unavailable without host callbacks")
{
    const imza::ToolOutput out = imza::test::run_lua(
        "imza.plan.create('# Requirements')\nprint('dead')", imza::LuaHost { });
    CHECK(out.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(out.text.find("plan.create: unavailable in this context")
        != std::string::npos);
    CHECK(out.text.find("dead") == std::string::npos);

    const imza::ToolOutput edit
        = imza::test::run_lua("imza.plan.edit('a', 'b')", imza::LuaHost { });
    CHECK(edit.text.find("plan.edit: unavailable in this context")
        != std::string::npos);
}

TEST_CASE("web bindings fail closed before argument validation")
{
    // The capability gate is enforced at registration (R3a), so a denied
    // binding raises even when called with missing arguments: access is
    // checked before the handler could report a type error.
    const imza::ToolOutput fetch
        = imza::test::run_lua("imza.web.fetch()\nprint('dead')");
    CHECK(fetch.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(fetch.text.find("web access is disabled") != std::string::npos);
    CHECK(fetch.text.find("dead") == std::string::npos);

    const imza::ToolOutput search
        = imza::test::run_lua("imza.web.search()\nprint('dead')");
    CHECK(search.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(search.text.find("web access is disabled") != std::string::npos);
}

TEST_CASE("imza.sh runs a single command and returns exit code")
{
    ShellFixture fx;
    const imza::ToolOutput out
        = imza::test::run_lua("local out, code = imza.shell('echo hello-sh')\n"
                              "print(out, code)",
            fx.host());
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text == "hello-sh\n    0\n");
    CHECK(fx.ask_calls == 0);

    const imza::ToolOutput failing = imza::test::run_lua(
        "local out, code = imza.shell('false')\nprint(out, code)", fx.host());
    CHECK(failing.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(failing.text == "    1\n");

    const imza::ToolOutput disabled
        = imza::test::run_lua("imza.shell('echo x')\nprint('dead')");
    CHECK(disabled.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(disabled.text.find("shell access is disabled") != std::string::npos);

    const imza::ToolOutput empty
        = imza::test::run_lua("imza.shell('')\nprint('dead')", fx.host());
    CHECK(empty.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(empty.text.find("empty command") != std::string::npos);
}

TEST_CASE("imza.sh workspace argument selects the run directory")
{
    imza::test::TempDir dir;
    ShellFixture fx;
    fx.workspace->working_directory = dir.path;

    const imza::ToolOutput absolute = imza::test::run_lua(
        "local out, code = imza.shell('pwd', 10, [[" + dir.path.string()
            + "]])\n"
              "print(out, code)",
        fx.host());
    CHECK(absolute.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(absolute.text.find(dir.path.string()) != std::string::npos);
    CHECK(absolute.text.find("    0\n") != std::string::npos);

    fs::create_directories(dir.file("sub"));
    const imza::ToolOutput relative
        = imza::test::run_lua("local out, code = imza.shell('pwd', 10, 'sub')\n"
                              "print(out, code)",
            fx.host());
    CHECK(relative.text.find((dir.file("sub")).string()) != std::string::npos);

    const imza::ToolOutput missing = imza::test::run_lua(
        "imza.shell('pwd', 10, 'no-such-dir')\nprint('dead')", fx.host());
    CHECK(missing.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(missing.text.find("not a directory") != std::string::npos);

    const imza::ToolOutput empty = imza::test::run_lua(
        "imza.shell('pwd', 10, '')\nprint('dead')", fx.host());
    CHECK(empty.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(empty.text.find("must not be empty") != std::string::npos);
}

TEST_CASE("imza.sh rejects only multiple command expressions")
{
    ShellFixture fx;
    const imza::ToolOutput chained = imza::test::run_lua(
        "imza.shell('echo a && echo b')\nprint('dead')", fx.host());
    CHECK(chained.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(chained.text.find("one command per call") != std::string::npos);
    CHECK(fx.ask_calls == 0);

    const imza::ToolOutput piped = imza::test::run_lua(
        "imza.shell('echo a | grep a')\nprint('dead')", fx.host());
    CHECK(piped.text.find("one command per call") != std::string::npos);

    const imza::ToolOutput multiline = imza::test::run_lua(
        "imza.shell('echo a\\necho b')\nprint('dead')", fx.host());
    CHECK(multiline.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(multiline.text.find("one command per call") != std::string::npos);
}

TEST_CASE("imza.sh routes redirects and non-catalog commands to the gate")
{
    const fs::path marker
        = fs::temp_directory_path() / "imza_sh_redirect_out.txt";
    ShellFixture unattended;
    unattended.attendable             = false;
    const imza::ToolOutput redirected = imza::test::run_lua(
        "imza.shell('echo hi > " + marker.string() + "')", unattended.host());
    CHECK(redirected.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(redirected.text.find("approval") != std::string::npos);

    const imza::ToolOutput expanded
        = imza::test::run_lua("imza.shell('echo $HOME')", unattended.host());
    CHECK(expanded.text.find("approval") != std::string::npos);

    const imza::ToolOutput mutating = imza::test::run_lua(
        "imza.shell('touch /tmp/imza_sh_unattended')", unattended.host());
    CHECK(mutating.text.find("approval") != std::string::npos);
}

TEST_CASE("imza.sh applies the native approval and session grant flow")
{
    imza::test::TempDir dir;

    ShellFixture once;
    once.verdict = imza::ToolVerdict { imza::ToolDecision::ACCEPT_ONCE, "" };
    const imza::ToolOutput accepted = imza::test::run_lua(
        "local out, code = imza.shell('echo $HOME')\nprint(code)", once.host());
    CHECK(accepted.text == "0\n");
    REQUIRE(once.ask_calls == 1);
    REQUIRE(once.last_prompt.has_value());
    CHECK(once.last_prompt->name == "shell");
    const auto& shell_request
        = std::get<imza::ShellRequest>(once.last_prompt->request);
    CHECK(shell_request.command == "echo $HOME");
    CHECK(shell_request.timeout == std::chrono::seconds(10));
    CHECK_FALSE(once.last_prompt->allow_for_session);
    CHECK(once.installed.empty());

    ShellFixture session;
    session.verdict
        = imza::ToolVerdict { imza::ToolDecision::ACCEPT_FOR_SESSION, "" };
    const imza::ToolOutput granted = imza::test::run_lua(
        "local out, code = imza.shell('touch " + dir.file("a").string()
            + "')\n"
              "print(code)",
        session.host());
    CHECK(granted.text == "0\n");
    REQUIRE(session.ask_calls == 1);
    REQUIRE(session.last_prompt.has_value());
    CHECK(session.last_prompt->name == "shell");
    CHECK(std::get<imza::ShellRequest>(session.last_prompt->request).command
        == "touch " + dir.file("a").string());
    CHECK(session.last_prompt->reason.find("touch") != std::string::npos);
    CHECK(session.last_prompt->allow_for_session);
    REQUIRE(session.installed.size() == 1);
    CHECK(std::get<imza::ShellCommandGrant>(session.installed.front()).program
        == "touch");

    ShellFixture rejected;
    rejected.verdict
        = imza::ToolVerdict { imza::ToolDecision::REJECT, "no thanks" };
    const imza::ToolOutput denied = imza::test::run_lua(
        "imza.shell('touch " + dir.file("b").string() + "')", rejected.host());
    REQUIRE(rejected.ask_calls == 1);
    CHECK(denied.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(denied.text.find("no thanks") != std::string::npos);
    CHECK(rejected.installed.empty());
}

TEST_CASE("imza.sh accepts pre-installed grants and skip-permissions silently")
{
    ShellFixture pre;
    CHECK(pre.install({ imza::ShellCommandGrant { "touch",
        fs::temp_directory_path().string() + "/imza_sh_pre_granted" } }));
    pre.verdict = imza::ToolVerdict { imza::ToolDecision::REJECT, "unused" };
    const imza::ToolOutput auto_run
        = imza::test::run_lua("local out, code = imza.shell('touch "
                + fs::temp_directory_path().string()
                + "/imza_sh_pre_granted')\n"
                  "print(code)",
            pre.host());
    CHECK(auto_run.text == "0\n");
    CHECK(pre.ask_calls == 0);

    ShellFixture skipped;
    skipped.skip_permissions = true;
    skipped.verdict
        = imza::ToolVerdict { imza::ToolDecision::REJECT, "unused" };
    const imza::ToolOutput bypassed
        = imza::test::run_lua("local out, code = imza.shell('touch "
                + fs::temp_directory_path().string()
                + "/imza_sh_skip')\n"
                  "print(code)",
            skipped.host());
    CHECK(bypassed.text == "0\n");
    CHECK(skipped.ask_calls == 0);
}

TEST_CASE("imza.tree.index lists declarations parsed by the grammar")
{
    imza::test::TempDir dir;
    imza::test::write_file(dir.file("sym.cpp"),
        "// not a symbol\n"
        "struct Alpha { int x; };\n"
        "int beta(int v) { return v; }\n"
        "class Gamma { };\n");

    const imza::ToolOutput out = imza::test::run_lua(
        "local rows = imza.tree.index([[" + dir.file("sym.cpp").string()
        + "]])\n"
          "for _, r in ipairs(rows) do print(r.kind, r.name, "
          "r.start_line, r.end_line) end");
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text.find("struct_specifier    Alpha    2    2\n")
        != std::string::npos);
    CHECK(out.text.find("function_definition    beta    3    3\n")
        != std::string::npos);
    CHECK(out.text.find("class_specifier    Gamma    4    4\n")
        != std::string::npos);
    // The comment is not a declaration and appears nowhere.
    CHECK(out.text.find("not a symbol") == std::string::npos);
}

TEST_CASE("imza.tree.index caps results and reports node text")
{
    imza::test::TempDir dir;
    std::string body;
    for (int i = 1; i <= 60; ++i) {
        body += "int fn" + std::to_string(i) + "() { return "
            + std::to_string(i) + "; }\n";
    }
    imza::test::write_file(dir.file("many.cpp"), body);

    const imza::ToolOutput out = imza::test::run_lua(
        "local rows = imza.tree.index([[" + dir.file("many.cpp").string()
        + "]])\nprint(#rows, rows[1].text)");
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(
        out.text.find("50    int fn1() { return 1; }\n") != std::string::npos);
}

TEST_CASE("imza.tree.nodes matches an exact type and reports node text")
{
    imza::test::TempDir dir;
    imza::test::write_file(dir.file("nodes.c"), "int main() { return 0; }\n");

    const imza::ToolOutput declared = imza::test::run_lua(
        "local rows = imza.tree.nodes([[" + dir.file("nodes.c").string()
        + "]], 'function_definition')\n"
          "print(#rows, rows[1].kind, rows[1].name)");
    CHECK(declared.text.find("1    function_definition    main\n")
        != std::string::npos);
}

TEST_CASE("imza.tree.nodes rejects unknown exact types")
{
    imza::test::TempDir dir;
    imza::test::write_file(dir.file("a.c"), "int main() { return 0; }\n");
    const imza::ToolOutput out = imza::test::run_lua("imza.tree.nodes([["
        + dir.file("a.c").string() + "]], 'not_a_real_node')\nprint('dead')");
    CHECK(out.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(out.text.find("unknown node type: not_a_real_node")
        != std::string::npos);
    CHECK(out.text.find("dead") == std::string::npos);
}

TEST_CASE("imza.tree.symbols lists identifier occurrences with lines")
{
    imza::test::TempDir dir;
    imza::test::write_file(dir.file("use.c"),
        "int cat;\n"
        "int dog;\n"
        "int use_cat() { return cat; }\n");

    const imza::ToolOutput out = imza::test::run_lua(
        "local rows = imza.tree.symbols([[" + dir.file("use.c").string()
        + "]], 'cat')\n"
          "for _, r in ipairs(rows) do print(r.file, r.line, r.text) end");
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    // Grammar-typed: the 'cat' inside 'use_cat' never matches.
    CHECK(out.text.find("    1    int cat;\n") != std::string::npos);
    // Grammar-typed: no row's line is exactly 'use_cat'; but its line 3
    // row exists because it contains the standalone `cat` identifier.
    CHECK(out.text.find("    3    int use_cat() { return cat; }\n")
        != std::string::npos);
}

TEST_CASE("imza.tree.references lists call sites of a symbol")
{
    imza::test::TempDir dir;
    imza::test::write_file(dir.file("use.c"),
        "int cat;\n"
        "int dog;\n"
        "int use_cat() { return cat; }\n"
        "int call_cat() { return cat(); }\n"
        "// cat() in a comment\n");

    const imza::ToolOutput out = imza::test::run_lua(
        "local rows = imza.tree.references([[" + dir.file("use.c").string()
        + "]], 'cat')\n"
          "for _, r in ipairs(rows) do print(r.line, r.kind, r.text) end");
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    // Only the call site on line 4 matches; the variable uses and the
    // comment are not identifier-in-call-node matches.
    CHECK(out.text
        == "4    call_expression    int call_cat() { return cat(); }\n");
}
TEST_CASE("ts bindings raise on bad paths and unknown grammars")
{
    imza::test::TempDir dir;
    imza::test::write_file(dir.file("a.c"), "int main() { return 0; }\n");
    imza::test::write_file(dir.file("note.unknownext"), "hello\n");

    const imza::ToolOutput missing = imza::test::run_lua(
        "imza.tree.index('no-such-file.c')\nprint('dead')");
    CHECK(missing.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(missing.text.find("no such file") != std::string::npos);
    CHECK(missing.text.find("looked for ") != std::string::npos);
    CHECK(missing.text.find("dead") == std::string::npos);

    const imza::ToolOutput nogrammar = imza::test::run_lua(
        "imza.tree.index([[" + dir.file("note.unknownext").string() + "]])");
    CHECK(nogrammar.text.find("no grammar") != std::string::npos);
}

TEST_CASE("filesystem bindings record a dispatch log with targets")
{
    imza::test::TempDir dir;
    imza::test::write_file(dir.file("a.txt"), "one\ntwo\n");
    const imza::ToolOutput out
        = imza::test::run_lua("imza.fs.read([[" + dir.file("a.txt").string()
            + "]])\n"
              "imza.fs.list([["
            + dir.path.string()
            + "]])\n"
              "imza.fs.grep([["
            + dir.file("a.txt").string() + "]], 'one')");
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    REQUIRE(out.dispatch_log.size() == 3);
    CHECK(out.dispatch_log[0].binding == "fs.read");
    CHECK(out.dispatch_log[0].ok);
    CHECK(out.dispatch_log[0].target == dir.file("a.txt").string());
    CHECK(out.dispatch_log[1].binding == "fs.list");
    CHECK(out.dispatch_log[1].ok);
    CHECK(out.dispatch_log[2].binding == "fs.grep");
    CHECK(out.dispatch_log[2].ok);
}

TEST_CASE("sh binding logs commands with exit status")
{
    imza::test::TempDir dir;
    ShellFixture fx;
    const imza::ToolOutput out
        = imza::test::run_lua("imza.shell('echo hi')\nimza.shell('false')\n"
                              "pcall(imza.shell, 'echo a && echo b')",
            fx.host());
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    // The pcall'd chain failure logs a third failed entry; argument
    // validation never reaches execution, so it carries no target.
    REQUIRE(out.dispatch_log.size() == 3);
    CHECK(out.dispatch_log[2].binding == "shell");
    CHECK(out.dispatch_log[2].target.empty());
    CHECK_FALSE(out.dispatch_log[2].ok);
    CHECK(out.dispatch_log[0].binding == "shell");
    CHECK(out.dispatch_log[0].target == "echo hi");
    CHECK(out.dispatch_log[0].ok);
    CHECK_FALSE(out.dispatch_log[1].ok);
}

TEST_CASE("a script killed at the deadline keeps its partial log")
{
    imza::test::TempDir dir;
    imza::test::write_file(dir.file("a.txt"), "x\n");
    const imza::ToolOutput out
        = imza::test::run_lua("imza.fs.read([[" + dir.file("a.txt").string()
            + "]])\n"
              "while true do end");
    CHECK(out.kind == imza::ToolOutput::Kind::ERROR);
    REQUIRE(out.dispatch_log.size() == 1);
    CHECK(out.dispatch_log[0].binding == "fs.read");
    CHECK(out.dispatch_log[0].ok);
}

TEST_CASE("imza.fs.insert inserts before a line and appends with nil")
{
    imza::test::TempDir dir;
    imza::test::write_file(dir.file("a.txt"), "one\ntwo\nthree\n");
    const std::string path = dir.file("a.txt").string();
    const imza::ToolOutput out
        = imza::test::run_lua("assert(not imza.fs.insert([[" + path
            + "]], 'inserted', 2))\n"
              "assert(not imza.fs.insert([["
            + path + "]], 'tail'))");
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(imza::test::read_all(dir.file("a.txt"))
        == "one\ninserted\ntwo\nthree\ntail\n");
    REQUIRE(out.diffs.size() == 1);
    CHECK(out.diffs[0].file == path);
}

TEST_CASE("imza.fs.insert rejects out-of-range lines and missing files")
{
    imza::test::TempDir dir;
    imza::test::write_file(dir.file("a.txt"), "one\ntwo\n");
    const std::string path     = dir.file("a.txt").string();
    const imza::ToolOutput out = imza::test::run_lua(
        "print(select(2, pcall(imza.fs.insert, [[" + path + "]], 'x', 10)))");
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text.find("exceeds file length") != std::string::npos);
    CHECK(out.diffs.empty());

    const imza::ToolOutput missing
        = imza::test::run_lua("print(select(2, pcall(imza.fs.insert, [["
            + dir.file("missing.txt").string() + "]], 'x')))");
    CHECK(missing.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(missing.text.find("no such file") != std::string::npos);
    CHECK(missing.text.find("use fs.write to create it") != std::string::npos);
}

TEST_CASE("imza.fs.edit replaces the first occurrence by default and all "
          "occurrences with count=0")
{
    imza::test::TempDir dir;
    imza::test::write_file(dir.file("a.txt"), "foo bar foo baz foo\n");
    const std::string path = dir.file("a.txt").string();

    SUBCASE("first occurrence by default")
    {
        const imza::ToolOutput out = imza::test::run_lua(
            "imza.fs.edit([[" + path + "]], 'foo', 'qux')");
        REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
        CHECK(
            imza::test::read_all(dir.file("a.txt")) == "qux bar foo baz foo\n");
    }
    SUBCASE("count=0 replaces all occurrences")
    {
        const imza::ToolOutput out = imza::test::run_lua(
            "imza.fs.edit([[" + path + "]], 'foo', 'qux', 0)");
        REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
        CHECK(
            imza::test::read_all(dir.file("a.txt")) == "qux bar qux baz qux\n");
    }
}

TEST_CASE("imza.fs.edit errors on missing match and empty old")
{
    imza::test::TempDir dir;
    imza::test::write_file(dir.file("a.txt"), "hello\n");
    const std::string path = dir.file("a.txt").string();
    const imza::ToolOutput out
        = imza::test::run_lua("print(select(2, pcall(imza.fs.edit, [[" + path
            + "]], 'absent', 'x')))");
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text.find("old text not found in file: \"absent\"; it must match")
        != std::string::npos);
    CHECK(out.text.find("must match exactly") != std::string::npos);
    CHECK(out.text.find(path + ":") != std::string::npos);
    CHECK(imza::test::read_all(dir.file("a.txt")) == "hello\n");

    const imza::ToolOutput empty_old = imza::test::run_lua(
        "print(select(2, pcall(imza.fs.edit, [[" + path + "]], '', 'x')))");
    CHECK(empty_old.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(empty_old.text.find("non-empty") != std::string::npos);
}

TEST_CASE("imza.fs.write creates and rewrites files")
{
    imza::test::TempDir dir;
    const std::string path     = dir.file("new.txt").string();
    const imza::ToolOutput out = imza::test::run_lua("imza.fs.write([[" + path
        + "]], 'v1\\n')\n"
          "imza.fs.write([["
        + path + "]], 'v2\\n')");
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(imza::test::read_all(dir.file("new.txt")) == "v2\n");
    // One net diff per file: original (empty) to latest.
    REQUIRE(out.diffs.size() == 1);
    CHECK(out.diffs[0].file == path);
}

TEST_CASE("imza.fs.write creates missing parent directories")
{
    imza::test::TempDir dir;
    const std::string path = dir.file("a/b/c.txt").string();
    const imza::ToolOutput out
        = imza::test::run_lua("imza.fs.write([[" + path + "]], 'deep\\n')");
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(imza::test::read_all(dir.file("a/b/c.txt")) == "deep\n");
    REQUIRE(out.diffs.size() == 1);
    CHECK(out.diffs[0].file == path);
}

TEST_CASE("imza.fs.write rejects a file occupying the parent path")
{
    WorkspaceFixture fx;
    imza::test::write_file(fx.dir.file("a"), "blocker\n");
    const std::string path     = fx.dir.file("a/b.txt").string();
    const imza::ToolOutput out = imza::test::run_lua(
        "imza.fs.write([[" + path + "]], 'x\\n')\nprint('dead')", fx.host());
    CHECK(out.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(
        out.text.find("target parent is not a directory") != std::string::npos);
    CHECK(imza::test::read_all(fx.dir.file("a")) == "blocker\n");
    CHECK(!fs::exists(fx.dir.file("a/b.txt")));
    CHECK(out.diffs.empty());
}

TEST_CASE("imza.fs.write with nested path fails closed unattended outside "
          "trusted roots")
{
    WorkspaceFixture fx;
    imza::test::TempDir outside;
    // No ask callback: ASK verdicts must fail closed.
    const std::string path     = outside.file("a/b/c.txt").string();
    const imza::ToolOutput out = imza::test::run_lua(
        "imza.fs.write([[" + path + "]], 'x\\n')", fx.host());
    CHECK(out.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(out.text.find("denied") != std::string::npos);
    CHECK(!fs::exists(outside.file("a")));
    CHECK(out.diffs.empty());
}

TEST_CASE("lua file mutations collapse into one net diff per file")
{
    imza::test::TempDir dir;
    imza::test::write_file(dir.file("a.txt"), "one\ntwo\nthree\n");
    imza::test::write_file(dir.file("b.txt"), "alpha\n");
    const std::string a        = dir.file("a.txt").string();
    const std::string b        = dir.file("b.txt").string();
    const imza::ToolOutput out = imza::test::run_lua("imza.fs.edit([[" + a
        + "]], 'two', 'TWO')\n"
          "imza.fs.insert([["
        + a
        + "]], 'zero', 1)\n"
          "imza.fs.edit([["
        + b + "]], 'alpha', 'ALPHA')");
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    REQUIRE(out.diffs.size() == 2);
    CHECK(out.diffs[0].file == a);
    CHECK(out.diffs[1].file == b);
    // Net diff localizes both edits: the "two"->"TWO" replacement and
    // the prepended "zero" become two small hunks.
    std::size_t adds    = 0;
    std::size_t removes = 0;
    for (const imza::DiffRow& row : out.diffs[0].rows) {
        if (row.kind == imza::DiffRow::Kind::ADD) {
            ++adds;
        }
        if (row.kind == imza::DiffRow::Kind::REMOVE) {
            ++removes;
        }
        CHECK(row.kind != imza::DiffRow::Kind::SKIP);
    }
    CHECK(adds == 2);
    CHECK(removes == 1);
}

TEST_CASE("lua file diffs survive a mid-script error")
{
    imza::test::TempDir dir;
    imza::test::write_file(dir.file("a.txt"), "one\n");
    const std::string a        = dir.file("a.txt").string();
    const imza::ToolOutput out = imza::test::run_lua("imza.fs.edit([[" + a
        + "]], 'one', 'ONE')\n"
          "error('boom')");
    CHECK(out.kind == imza::ToolOutput::Kind::ERROR);
    REQUIRE(out.diffs.size() == 1);
    CHECK(out.diffs[0].file == a);
    CHECK(imza::test::read_all(dir.file("a.txt")) == "ONE\n");
}

TEST_CASE("a failed fs.edit aborts mid-script leaving the last good state")
{
    imza::test::TempDir dir;
    imza::test::write_file(dir.file("a.txt"), "one\n");
    imza::test::write_file(dir.file("b.txt"), "keep\n");
    const std::string a        = dir.file("a.txt").string();
    const std::string b        = dir.file("b.txt").string();
    const imza::ToolOutput out = imza::test::run_lua("imza.fs.edit([[" + a
        + "]], 'one', 'ONE')\n"
          "imza.fs.edit([["
        + b
        + "]], 'absent', 'x')\n"
          "print('dead')");
    CHECK(out.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(out.text.find("old text not found") != std::string::npos);
    CHECK(out.text.find("dead") == std::string::npos);
    CHECK(imza::test::read_all(dir.file("a.txt")) == "ONE\n");
    // The abort happened before b.txt was touched.
    CHECK(imza::test::read_all(dir.file("b.txt")) == "keep\n");
}

TEST_CASE("pcall around a failed fs.edit recovers")
{
    imza::test::TempDir dir;
    imza::test::write_file(dir.file("a.txt"), "one\n");
    const std::string path = dir.file("a.txt").string();
    const imza::ToolOutput out
        = imza::test::run_lua("if pcall(imza.fs.edit, [[" + path
            + "]], 'absent', 'x') then\n"
              "  print('unexpected success')\n"
              "else\n"
              "  imza.fs.edit([["
            + path
            + "]], 'one', 'ONE')\n"
              "end\n"
              "return imza.fs.read([["
            + path + "]])");
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(imza::test::read_all(dir.file("a.txt")) == "ONE\n");
    REQUIRE(out.return_value.has_value());
    CHECK(out.return_value->as<std::string>() == "ONE\n");
}

TEST_CASE("a denied write aborts and the dispatch log records the denial")
{
    WorkspaceFixture fx;
    imza::test::TempDir outside;
    // No ask callback: ASK verdicts fail closed.
    const std::string path     = outside.file("a.txt").string();
    const imza::ToolOutput out = imza::test::run_lua("imza.fs.write([[" + path
            + "]], 'x\\n')\n"
              "print('dead')",
        fx.host());
    CHECK(out.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(out.text.find("denied") != std::string::npos);
    CHECK(out.text.find("dead") == std::string::npos);
    REQUIRE(out.dispatch_log.size() == 1);
    CHECK(out.dispatch_log[0].binding == "fs.write");
    CHECK(out.dispatch_log[0].target == path);
    CHECK_FALSE(out.dispatch_log[0].ok);
    CHECK(!fs::exists(outside.file("a.txt")));
}

TEST_CASE("imza.file mutations auto-accept in trusted Build mode")
{
    WorkspaceFixture fx;
    imza::test::write_file(fx.dir.file("a.txt"), "one\n");
    const std::string a        = fx.dir.file("a.txt").string();
    const imza::ToolOutput out = imza::test::run_lua(
        "imza.fs.edit([[" + a + "]], 'one', 'ONE')", fx.host());
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(imza::test::read_all(fx.dir.file("a.txt")) == "ONE\n");
}

TEST_CASE("imza.file mutations outside the workspace follow the ask verdict")
{
    WorkspaceFixture fx;
    imza::test::TempDir outside;
    imza::test::write_file(outside.file("a.txt"), "one\n");
    const std::string a = outside.file("a.txt").string();

    SUBCASE("unattended: the ask fails closed")
    {
        // No ask callback: ASK verdicts must fail closed.
        const imza::ToolOutput out = imza::test::run_lua(
            "imza.fs.edit([[" + a + "]], 'one', 'ONE')", fx.host());
        CHECK(out.kind == imza::ToolOutput::Kind::ERROR);
        CHECK(out.text.find("denied") != std::string::npos);
        CHECK(imza::test::read_all(outside.file("a.txt")) == "one\n");
    }

    SUBCASE("attended: approval lets the edit proceed")
    {
        imza::LuaHost host = fx.host();
        host.ask           = [&](imza::ModalPayload payload) {
            const auto& prompt = std::get<imza::PermissionPrompt>(payload);
            CHECK(prompt.name == "edit");
            CHECK(prompt.target == outside.file("a.txt").string());
            const auto& edit = std::get<imza::EditFileRequest>(
                std::get<imza::FilesystemRequest>(prompt.request));
            CHECK(edit.old_text == "one");
            CHECK(edit.new_text == "ONE");
            CHECK_FALSE(prompt.reason.empty());
            std::promise<imza::ModalResult> promise;
            promise.set_value(imza::ModalResult {
                imza::ToolVerdict { imza::ToolDecision::ACCEPT_ONCE, "" } });
            return promise.get_future();
        };
        const imza::ToolOutput out = imza::test::run_lua(
            "imza.fs.edit([[" + a + "]], 'one', 'ONE')", std::move(host));
        REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
        CHECK(imza::test::read_all(outside.file("a.txt")) == "ONE\n");
    }
}

TEST_CASE("canvas line emits a declarative chart and returns nothing")
{
    const imza::ToolOutput out = imza::test::run_lua(
        "return imza.canvas.line({ title = 'CPU', data = { 1, 2, 3 } })");
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK_FALSE(out.return_value.has_value());
    REQUIRE(out.canvases.size() == 1);
    const imza::CanvasView& chart = out.canvases[0];
    CHECK(chart.kind == imza::CanvasView::Kind::LINE);
    CHECK(chart.title == "CPU");
    REQUIRE(chart.series.size() == 1);
    REQUIRE(chart.series[0].values.size() == 3);
    CHECK(chart.series[0].values[2] == doctest::Approx(3.0));
    REQUIRE(out.dispatch_log.size() == 1);
    CHECK(out.dispatch_log[0].binding == "canvas.line");
    CHECK(out.dispatch_log[0].target == "CPU");
    CHECK(out.dispatch_log[0].ok);
}

TEST_CASE("canvas line accepts named series")
{
    const imza::ToolOutput out = imza::test::run_lua(R"lua(
return imza.canvas.line({
  title = 'Trend',
  data = {
    { label = 'a', values = { 1, 2 } },
    { label = 'b', values = { 3, 4 } },
  },
})
)lua");
    REQUIRE(out.canvases.size() == 1);
    REQUIRE(out.canvases[0].series.size() == 2);
    CHECK(out.canvases[0].series[0].label == "a");
    CHECK(out.canvases[0].series[1].values[1] == doctest::Approx(4.0));
}

TEST_CASE("canvas bar and pie take labeled points")
{
    const imza::ToolOutput out = imza::test::run_lua(R"lua(
local bar = imza.canvas.bar({
  title = 'Fruit',
  data = { { label = 'apple', value = 3 }, { label = 'pear', value = 5 } },
})
local pie = imza.canvas.pie({
  data = { { label = 'a', value = 1 }, { label = 'b', value = 2 } },
})
)lua");
    REQUIRE(out.canvases.size() == 2);
    CHECK(out.canvases[0].kind == imza::CanvasView::Kind::BAR);
    CHECK(out.canvases[0].title == "Fruit");
    CHECK(out.canvases[0].series[1].label == "pear");
    CHECK(out.canvases[0].series[1].values[0] == doctest::Approx(5.0));
    CHECK(out.canvases[1].kind == imza::CanvasView::Kind::PIE);
}

TEST_CASE("canvas rejects malformed and non-positive chart data")
{
    const imza::ToolOutput out = imza::test::run_lua(R"lua(
print(select(2, pcall(imza.canvas.pie,
  { data = { { label = 'x', value = -1 } } })))
print(select(2, pcall(imza.canvas.bar, { data = { { label = 'x' } } })))
print(select(2, pcall(imza.canvas.line, { data = { 1, 'x' } })))
print(select(2, pcall(imza.canvas.surface, { data = { { 1, 2 }, { 3 } } })))
)lua");
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(
        out.text.find("canvas.pie: 'data' point 1 needs a 'label' string and a "
                      "finite positive 'value' number")
        != std::string::npos);
    CHECK(
        out.text.find("canvas.bar: 'data' point 1 needs a 'label' string and a "
                      "finite 'value' number")
        != std::string::npos);
    CHECK(out.text.find("canvas.line: 'data' entry 2 must be a finite number")
        != std::string::npos);
    CHECK(out.text.find("canvas.surface: row 2 must have 2 columns")
        != std::string::npos);
    CHECK(out.canvases.empty());
}

TEST_CASE("canvas surface takes a rectangular z grid")
{
    const imza::ToolOutput out = imza::test::run_lua(
        "return imza.canvas.surface("
        "{ title = 'Z', data = { { 1, 2, 3 }, { 4, 5, 6 } } })");
    REQUIRE(out.canvases.size() == 1);
    CHECK(out.canvases[0].kind == imza::CanvasView::Kind::SURFACE);
    REQUIRE(out.canvases[0].grid.size() == 2);
    CHECK(out.canvases[0].grid[1][2] == doctest::Approx(6.0));
}

TEST_CASE("canvas caps charts per run and fails closed")
{
    const imza::ToolOutput out = imza::test::run_lua(R"lua(
for _ = 1, 16 do
  imza.canvas.line({ data = { 1, 2 } })
end
print(select(2, pcall(imza.canvas.line, { data = { 1, 2 } })))
)lua");
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text.find("canvas.line: too many charts in one run")
        != std::string::npos);
    CHECK(out.canvases.size() == 16);
}
