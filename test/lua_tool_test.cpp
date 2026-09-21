#include <doctest/doctest.h>
#include <json/json.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include "network/json_io.h"
#include "permissions/store.h"
#include "tools/tool.h"
#include "workspace/environment.h"

namespace fs = std::filesystem;

namespace {

int next_dir_id()
{
    static int n = 0;
    return ++n;
}

struct TmpDir {
    fs::path path;

    TmpDir()
        : path(fs::temp_directory_path()
              / ("imza_lua_test_" + std::to_string(next_dir_id())))
    {
        fs::create_directories(path);
    }

    ~TmpDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    fs::path file(const std::string& name) const { return path / name; }
};

void write_file(const fs::path& p, const std::string& body)
{
    std::ofstream out(p);
    out << body;
}

imza::ToolOutput run_script(const std::string& script, imza::LuaHost host = { })
{
    const imza::Tool tool = imza::make_lua_tool(std::move(host));
    imza::ToolCallRequest req;
    req.name = "lua";
    Json::Value args;
    args["script"] = script;
    req.args       = imza::write_json(args);
    return imza::dispatch_tool({ &tool, 1 }, req);
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

} // namespace

TEST_CASE("lua tool prints values to its output")
{
    const imza::ToolOutput out = run_script("print('hello', 42, true)\n"
                                            "local t = {3, 1, 2}\n"
                                            "table.sort(t)\n"
                                            "print(table.concat(t, ','))");
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text == "hello\t42\ttrue\n1,2,3\n");
}

TEST_CASE("binding errors split: operational failures are values, type "
          "mistakes raise")
{
    // An operational failure (missing file) is the documented nil, err
    // value; the script survives and keeps running.
    const imza::ToolOutput value_error
        = run_script("local data, err = tool.read('no-such-file.txt')\n"
                     "print(data == nil, err ~= nil)");
    CHECK(value_error.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(value_error.text == "true\ttrue\n");

    // A wrong argument type is a script bug: it raises with a Lua error
    // position and aborts the run. (Numbers coerce to strings per the Lua
    // C API; nil, booleans, and tables are the rejected shapes.)
    const imza::ToolOutput raised = run_script("tool.read({})\nprint('dead')");
    CHECK(raised.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(raised.text.find("bad argument #1 to 'read'") != std::string::npos);
    CHECK(raised.text.find("string expected, got table") != std::string::npos);
    CHECK(raised.text.find("dead") == std::string::npos);

    // pcall is the sanctioned way to survive a type mistake.
    const imza::ToolOutput caught
        = run_script("print(pcall(tool.read, {}) == false)");
    CHECK(caught.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(caught.text == "true\n");
}

TEST_CASE("lua tool rejects empty scripts and reports errors with positions")
{
    CHECK(run_script("").kind == imza::ToolOutput::Kind::ERROR);

    const imza::ToolOutput runtime = run_script("error('boom')");
    CHECK(runtime.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(runtime.text.find("boom") != std::string::npos);

    const imza::ToolOutput syntax = run_script("retuern 1");
    CHECK(syntax.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(syntax.text.find(":1:") != std::string::npos);
}

TEST_CASE("lua sandbox denies io, os, and binary chunks")
{
    CHECK(run_script("io.read()").text.find("attempt to index a nil value")
        != std::string::npos);

    CHECK(run_script("os.execute('touch /tmp/imza_pwn')").kind
        == imza::ToolOutput::Kind::ERROR);

    const imza::ToolOutput chunk
        = run_script("print(load('\\27Lua binary') == nil)");
    CHECK(chunk.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(chunk.text.find("true") != std::string::npos);
}

TEST_CASE("lua tool kills runaway loops at the deadline")
{
    const imza::ToolOutput out
        = run_script(R"({"script":"while true do end","timeout":1})"
                     "\nfoo");
    CHECK(out.kind == imza::ToolOutput::Kind::ERROR);
}

TEST_CASE("lua tool truncates oversized output")
{
    const imza::ToolOutput out
        = run_script("for i = 1, 100000 do print(i) end");
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text.size() < 100 * 1024);
    CHECK(out.text.find("[truncated]") != std::string::npos);
}

TEST_CASE("lua tool captures top-level return values as JSON")
{
    const imza::ToolOutput none = run_script("print('x')");
    CHECK(none.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(none.text == "x\n");
    CHECK_FALSE(none.return_value.has_value());

    const imza::ToolOutput object = run_script(
        "print('log')\n"
        "return { file = 'a.cpp', changed = true, lines = { 10, 11 } }");
    CHECK(object.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(object.text == "log\n");
    REQUIRE(object.return_value.has_value());
    CHECK((*object.return_value)["file"].asString() == "a.cpp");
    CHECK((*object.return_value)["changed"].asBool());
    CHECK((*object.return_value)["lines"].size() == 2);
    CHECK((*object.return_value)["lines"][1].asInt() == 11);

    const imza::ToolOutput scalars = run_script("return 42");
    CHECK(scalars.return_value.has_value());
    CHECK(scalars.return_value->asInt() == 42);

    const imza::ToolOutput null = run_script("return nil");
    CHECK(null.return_value.has_value());
    CHECK(null.return_value->isNull());

    const imza::ToolOutput multi = run_script("return 'a', 2, false");
    REQUIRE(multi.return_value.has_value());
    REQUIRE(multi.return_value->isArray());
    CHECK(multi.return_value->size() == 3);
    CHECK((*multi.return_value)[0].asString() == "a");
    CHECK((*multi.return_value)[1].asInt() == 2);
    CHECK_FALSE((*multi.return_value)[2].asBool());

    const imza::ToolOutput empty = run_script("return {}");
    REQUIRE(empty.return_value.has_value());
    CHECK(empty.return_value->isObject());
    CHECK(empty.return_value->empty());
}

TEST_CASE("lua tool rejects unconvertible return values")
{
    const auto rejected
        = [](const std::string& script, const std::string& reason) {
              const imza::ToolOutput out = run_script(script);
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
    TmpDir dir;
    write_file(dir.file("a.txt"), "one\n");
    const std::string script = "tool.file.write('" + dir.file("a.txt").string()
        + "', 'two\\n')\n"
          "return function() end";
    const imza::ToolOutput out = run_script(script);
    CHECK(out.kind == imza::ToolOutput::Kind::ERROR);
    CHECK_FALSE(out.return_value.has_value());
    REQUIRE(out.dispatch_log.size() == 1);
    CHECK(out.dispatch_log[0].binding == "file.write");
    REQUIRE(out.diffs.size() == 1);
    CHECK(out.diffs[0].file == dir.file("a.txt").string());
}

TEST_CASE("lua print truncation respects the hard output cap")
{
    const imza::ToolOutput out
        = run_script("for i = 1, 100000 do print(i, 'x', true) end");
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text.size() <= 64 * 1024);
    CHECK(out.text.find("[truncated]") != std::string::npos);
}

TEST_CASE("tool.read returns 1-based windows and empty for empty files")
{
    TmpDir dir;
    write_file(dir.file("lines.txt"), "alpha\nbeta\ngamma\n");
    write_file(dir.file("empty.txt"), "");

    const imza::ToolOutput full = run_script("local s, e = tool.read([["
        + dir.file("lines.txt").string()
        + "]])\n"
          "print(s)");
    CHECK(full.text == "alpha\nbeta\ngamma\n\n");

    const imza::ToolOutput window = run_script("local s = tool.read([["
        + dir.file("lines.txt").string() + "]], 2, 3)\nprint(s)");
    CHECK(window.text == "beta\ngamma\n\n");

    const imza::ToolOutput empty
        = run_script("local s = tool.read([[" + dir.file("empty.txt").string()
            + "]])\n"
              "print(s == '')");
    CHECK(empty.text == "true\n");

    const imza::ToolOutput bad_range = run_script("local s, e = tool.read([["
        + dir.file("lines.txt").string() + "]], 0)\nprint(e)");
    CHECK(bad_range.text.find("1-based") != std::string::npos);
}

TEST_CASE("tool.list returns entries with type and size fields")
{
    TmpDir dir;
    write_file(dir.file("visible.txt"), "content");
    fs::create_directories(dir.file("sub"));

    const imza::ToolOutput out
        = run_script("local rows, err = tool.list([[" + dir.path.string()
            + "]])\n"
              "if err then error(err) end\n"
              "for _, r in ipairs(rows) do print(r.path, r.type) end");
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text.find("visible.txt\tfile") != std::string::npos);
    CHECK(out.text.find("sub\tdir") != std::string::npos);
}

TEST_CASE("tool.grep returns file, line, and text per match")
{
    TmpDir dir;
    write_file(dir.file("hay.cpp"), "int cat = 1;\nint dog = 2;\ncat();\n");

    const imza::ToolOutput out
        = run_script("local rows, err = tool.grep([[" + dir.path.string()
            + "]], 'cat')\n"
              "if err then error(err) end\n"
              "print(#rows, rows[1].file, rows[1].line, rows[1].text)\n"
              "for _, r in ipairs(rows) do print(r.file, r.line, r.text) end");
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text.find("hay.cpp\t1\tint cat = 1;") != std::string::npos);
    CHECK(out.text.find("hay.cpp\t3\tcat();") != std::string::npos);
}

TEST_CASE("tool.grep accepts a single file target and fills the file field")
{
    TmpDir dir;
    write_file(dir.file("one.cpp"), "only match here\nnope\n");

    const imza::ToolOutput file_out = run_script(
        "local rows, err = tool.grep([[" + dir.file("one.cpp").string()
        + "]], 'match')\n"
          "if err then error(err) end\n"
          "if #rows ~= 1 then error('expected 1 row') end\n"
          "print(rows[1].file, rows[1].line, rows[1].text)");
    CHECK(file_out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(
        file_out.text.find("one.cpp\t1\tonly match here") != std::string::npos);
}

TEST_CASE("bindings outside the workspace return an error value")
{
    TmpDir dir;
    // Without a provider the binding runs in trusted mode; with a provider
    // whose context lacks workspace/system it must reject, not crash.
    imza::LuaHost empty { };
    empty.permission_context   = [] { return imza::PermissionContext { }; };
    const imza::ToolOutput out = run_script("local s, e = tool.read([["
            + dir.file("lines.txt").string() + "]])\nprint(s, e)",
        std::move(empty));
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text.find("permission denied") != std::string::npos);
}

TEST_CASE("todo bindings read and write the shared task board")
{
    imza::LuaHost host { };
    imza::TodoList board;
    host.todo     = [&board] { return board; };
    host.set_todo = [&board](imza::TodoList todo) { board = std::move(todo); };

    const imza::ToolOutput set
        = run_script("local ok, err = tool.todo.set({"
                     "{content = 'first', status = 'in_progress'},"
                     "{content = 'second'}})\n"
                     "if err then error(err) end\nprint(ok)",
            std::move(host));
    CHECK(set.kind == imza::ToolOutput::Kind::OUTPUT);
    REQUIRE(board.items.size() == 2);
    CHECK(board.items[0].content == "first");
    CHECK(board.items[0].status == imza::TodoItem::Status::IN_PROGRESS);
    CHECK(board.items[1].status == imza::TodoItem::Status::PENDING);

    imza::LuaHost host2 { };
    host2.todo     = [&board] { return board; };
    host2.set_todo = [&board](imza::TodoList todo) { board = std::move(todo); };
    const imza::ToolOutput get
        = run_script("local rows = tool.todo.get()\n"
                     "print(#rows, rows[1].content, rows[1].status, "
                     "rows[2].status)",
            std::move(host2));
    CHECK(get.text == "2\tfirst\tin_progress\tpending\n");

    const imza::ToolOutput bad
        = run_script("local ok, err = tool.todo.set({"
                     "{content = 'x', status = 'nope'}})\nprint(err)",
            imza::LuaHost { });
    CHECK(bad.text.find("unknown status") != std::string::npos);
}

TEST_CASE("todo bindings round-trip the cancelled status")
{
    imza::LuaHost host { };
    imza::TodoList board;
    host.todo     = [&board] { return board; };
    host.set_todo = [&board](imza::TodoList todo) { board = std::move(todo); };

    const imza::ToolOutput set
        = run_script("local ok, err = tool.todo.set({"
                     "{content = 'done', status = 'completed'},"
                     "{content = 'dropped', status = 'cancelled'}})\n"
                     "if err then error(err) end",
            std::move(host));
    CHECK(set.kind == imza::ToolOutput::Kind::OUTPUT);
    REQUIRE(board.items.size() == 2);
    CHECK(board.items[0].status == imza::TodoItem::Status::COMPLETED);
    CHECK(board.items[1].status == imza::TodoItem::Status::CANCELLED);

    imza::LuaHost host2 { };
    host2.todo     = [&board] { return board; };
    host2.set_todo = [&board](imza::TodoList todo) { board = std::move(todo); };
    const imza::ToolOutput get
        = run_script("local rows = tool.todo.get()\n"
                     "print(rows[1].status, rows[2].status)",
            std::move(host2));
    CHECK(get.text == "completed\tcancelled\n");
}

TEST_CASE("tool.ask surfaces answers and unattended runs reject")
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

    const imza::ToolOutput out
        = run_script("local rows, err = tool.ask({{prompt = 'deploy?', "
                     "options = {'yes', 'no'}}})\n"
                     "if err then error(err) end\n"
                     "print(rows[1].question, rows[1].answer)",
            std::move(host));
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text == "deploy?\tyes\n");

    const imza::ToolOutput unattended = run_script(
        "local rows, err = tool.ask({{prompt = 'hello?'}})\nprint(err)");
    CHECK(unattended.text.find("unavailable") != std::string::npos);
}

TEST_CASE("web bindings fail closed without web access")
{
    const imza::ToolOutput fetch = run_script(
        "local body, err = tool.web.fetch('https://example.com')\nprint(err)");
    CHECK(fetch.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(fetch.text.find("web access is disabled") != std::string::npos);

    const imza::ToolOutput search
        = run_script("local body, err = tool.web.search('imza')\nprint(err)");
    CHECK(search.text.find("web access is disabled") != std::string::npos);
}

TEST_CASE("web bindings fail closed before argument validation")
{
    // The capability gate is enforced at registration (R3a), so a denied
    // binding returns `nil, err` even when called with missing arguments:
    // access is checked before the handler could raise a type error.
    const imza::ToolOutput fetch
        = run_script("local body, err = tool.web.fetch()\nprint(err)");
    CHECK(fetch.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(fetch.text.find("web access is disabled") != std::string::npos);

    const imza::ToolOutput search
        = run_script("local body, err = tool.web.search()\nprint(err)");
    CHECK(search.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(search.text.find("web access is disabled") != std::string::npos);
}

TEST_CASE("tool.sh runs a single command and returns exit code")
{
    ShellFixture fx;
    const imza::ToolOutput out
        = run_script("local out, code = tool.shell('echo hello-sh')\n"
                     "print(out, code)",
            fx.host());
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text == "hello-sh\n\t0\n");
    CHECK(fx.ask_calls == 0);

    const imza::ToolOutput failing = run_script(
        "local out, code = tool.shell('false')\nprint(out, code)", fx.host());
    CHECK(failing.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(failing.text == "\t1\n");

    const imza::ToolOutput disabled
        = run_script("local out, err = tool.shell('echo x')\nprint(err)");
    CHECK(disabled.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(disabled.text.find("shell access is disabled") != std::string::npos);

    const imza::ToolOutput empty
        = run_script("local out, err = tool.shell('')\nprint(err)", fx.host());
    CHECK(empty.text.find("empty command") != std::string::npos);
}

TEST_CASE("tool.sh workspace argument selects the run directory")
{
    TmpDir dir;
    ShellFixture fx;
    fx.workspace->working_directory = dir.path;

    const imza::ToolOutput absolute = run_script(
        "local out, code = tool.shell('pwd', 10, [[" + dir.path.string()
            + "]])\n"
              "print(out, code)",
        fx.host());
    CHECK(absolute.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(absolute.text.find(dir.path.string()) != std::string::npos);
    CHECK(absolute.text.find("\t0\n") != std::string::npos);

    fs::create_directories(dir.file("sub"));
    const imza::ToolOutput relative
        = run_script("local out, code = tool.shell('pwd', 10, 'sub')\n"
                     "print(out, code)",
            fx.host());
    CHECK(relative.text.find((dir.file("sub")).string()) != std::string::npos);

    const imza::ToolOutput missing = run_script(
        "local out, err = tool.shell('pwd', 10, 'no-such-dir')\nprint(err)",
        fx.host());
    CHECK(missing.text.find("not a directory") != std::string::npos);

    const imza::ToolOutput empty = run_script(
        "local out, err = tool.shell('pwd', 10, '')\nprint(err)", fx.host());
    CHECK(empty.text.find("must not be empty") != std::string::npos);
}

TEST_CASE("tool.sh rejects only multiple command expressions")
{
    ShellFixture fx;
    const imza::ToolOutput chained = run_script(
        "local out, err = tool.shell('echo a && echo b')\nprint(err)",
        fx.host());
    CHECK(chained.text.find("one command per call") != std::string::npos);
    CHECK(fx.ask_calls == 0);

    const imza::ToolOutput piped = run_script(
        "local out, err = tool.shell('echo a | grep a')\nprint(err)",
        fx.host());
    CHECK(piped.text.find("one command per call") != std::string::npos);

    const imza::ToolOutput multiline = run_script(
        "local out, err = tool.shell('echo a\\necho b')\nprint(err)",
        fx.host());
    CHECK(multiline.text.find("one command per call") != std::string::npos);
}

TEST_CASE("tool.sh routes redirects and non-catalog commands to the gate")
{
    const fs::path marker
        = fs::temp_directory_path() / "imza_sh_redirect_out.txt";
    ShellFixture unattended;
    unattended.attendable = false;
    const imza::ToolOutput redirected
        = run_script("local out, err = tool.shell('echo hi > " + marker.string()
                + "')\n"
                  "print(err)",
            unattended.host());
    CHECK(redirected.text.find("approval") != std::string::npos);

    const imza::ToolOutput expanded
        = run_script("local out, err = tool.shell('echo $HOME')\nprint(err)",
            unattended.host());
    CHECK(expanded.text.find("approval") != std::string::npos);

    const imza::ToolOutput mutating = run_script(
        "local out, err = tool.shell('touch /tmp/imza_sh_unattended')\n"
        "print(err)",
        unattended.host());
    CHECK(mutating.text.find("approval") != std::string::npos);
}

TEST_CASE("tool.sh applies the native approval and session grant flow")
{
    const fs::path dir = fs::temp_directory_path() / "imza_sh_grants";
    fs::create_directories(dir);

    ShellFixture once;
    once.verdict = imza::ToolVerdict { imza::ToolDecision::ACCEPT_ONCE, "" };
    const imza::ToolOutput accepted = run_script(
        "local out, code = tool.shell('echo $HOME')\nprint(code)", once.host());
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
    const imza::ToolOutput granted = run_script(
        "local out, code = tool.shell('touch " + (dir / "a").string()
            + "')\n"
              "print(code)",
        session.host());
    CHECK(granted.text == "0\n");
    REQUIRE(session.ask_calls == 1);
    REQUIRE(session.last_prompt.has_value());
    CHECK(session.last_prompt->name == "shell");
    CHECK(std::get<imza::ShellRequest>(session.last_prompt->request).command
        == "touch " + (dir / "a").string());
    CHECK(session.last_prompt->reason.find("touch") != std::string::npos);
    CHECK(session.last_prompt->allow_for_session);
    REQUIRE(session.installed.size() == 1);
    CHECK(std::get<imza::ShellCommandGrant>(session.installed.front()).program
        == "touch");

    ShellFixture rejected;
    rejected.verdict
        = imza::ToolVerdict { imza::ToolDecision::REJECT, "no thanks" };
    const imza::ToolOutput denied = run_script(
        "local out, err = tool.shell('touch " + (dir / "b").string()
            + "')\n"
              "print(err)",
        rejected.host());
    REQUIRE(rejected.ask_calls == 1);
    CHECK(denied.text.find("no thanks") != std::string::npos);
    CHECK(rejected.installed.empty());

    std::error_code error_cleanup;
    fs::remove_all(dir, error_cleanup);
}

TEST_CASE("tool.sh accepts pre-installed grants and skip-permissions silently")
{
    ShellFixture pre;
    CHECK(pre.install({ imza::ShellCommandGrant { "touch",
        fs::temp_directory_path().string() + "/imza_sh_pre_granted" } }));
    pre.verdict = imza::ToolVerdict { imza::ToolDecision::REJECT, "unused" };
    const imza::ToolOutput auto_run
        = run_script("local out, code = tool.shell('touch "
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
        = run_script("local out, code = tool.shell('touch "
                + fs::temp_directory_path().string()
                + "/imza_sh_skip')\n"
                  "print(code)",
            skipped.host());
    CHECK(bypassed.text == "0\n");
    CHECK(skipped.ask_calls == 0);
}

TEST_CASE("tool.ts.index lists declarations parsed by the grammar")
{
    TmpDir dir;
    write_file(dir.file("sym.cpp"),
        "// not a symbol\n"
        "struct Alpha { int x; };\n"
        "int beta(int v) { return v; }\n"
        "class Gamma { };\n");

    const imza::ToolOutput out = run_script("local rows, err = tool.ts.index([["
        + dir.file("sym.cpp").string()
        + "]])\n"
          "if err then error(err) end\n"
          "for _, r in ipairs(rows) do print(r.kind, r.name, "
          "r.start_line, r.end_line) end");
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(
        out.text.find("struct_specifier\tAlpha\t2\t2\n") != std::string::npos);
    CHECK(out.text.find("function_definition\tbeta\t3\t3\n")
        != std::string::npos);
    CHECK(out.text.find("class_specifier\tGamma\t4\t4\n") != std::string::npos);
    // The comment is not a declaration and appears nowhere.
    CHECK(out.text.find("not a symbol") == std::string::npos);
}

TEST_CASE("tool.ts.index caps results and reports node text")
{
    TmpDir dir;
    std::string body;
    for (int i = 1; i <= 60; ++i) {
        body += "int fn" + std::to_string(i) + "() { return "
            + std::to_string(i) + "; }\n";
    }
    write_file(dir.file("many.cpp"), body);

    const imza::ToolOutput out = run_script("local rows, err = tool.ts.index([["
        + dir.file("many.cpp").string()
        + "]])\n"
          "if err then error(err) end\nprint(#rows, rows[1].text)");
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text.find("50\tint fn1() { return 1; }\n") != std::string::npos);
}

TEST_CASE("tool.ts.nodes matches exact types and globs")
{
    TmpDir dir;
    write_file(dir.file("nodes.c"), "int main() { return 0; }\n");

    const imza::ToolOutput exact = run_script(
        "local rows, err = tool.ts.nodes([[" + dir.file("nodes.c").string()
        + "]], 'call_expression')\n"
          "if err then error(err) end\nprint(#rows)");
    CHECK(exact.text == "0\n");

    const imza::ToolOutput glob = run_script(
        "local rows, err = tool.ts.nodes([[" + dir.file("nodes.c").string()
        + "]], '*call*')\n"
          "if err then error(err) end\nprint(#rows)");
    CHECK(glob.text == "0\n");

    const imza::ToolOutput declared = run_script(
        "local rows, err = tool.ts.nodes([[" + dir.file("nodes.c").string()
        + "]], 'function_definition')\n"
          "if err then error(err) end\n"
          "print(#rows, rows[1].kind, rows[1].name)");
    CHECK(declared.text.find("1\tfunction_definition\tmain\n")
        != std::string::npos);
}

TEST_CASE("tool.ts.nodes rejects unknown exact types")
{
    TmpDir dir;
    write_file(dir.file("a.c"), "int main() { return 0; }\n");
    const imza::ToolOutput out = run_script("local rows, err = tool.ts.nodes([["
        + dir.file("a.c").string()
        + "]], 'not_a_real_node')\nprint(rows, err)");
    CHECK(out.text.find("not_a_real_node") != std::string::npos);
}

TEST_CASE("tool.ts.symbols lists identifier occurrences with lines")
{
    TmpDir dir;
    write_file(dir.file("use.c"),
        "int cat;\n"
        "int dog;\n"
        "int use_cat() { return cat; }\n");

    const imza::ToolOutput out = run_script(
        "local rows, err = tool.ts.symbols([[" + dir.file("use.c").string()
        + "]], 'cat')\n"
          "if err then error(err) end\n"
          "for _, r in ipairs(rows) do print(r.file, r.line, r.text) end");
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    // Grammar-typed: the 'cat' inside 'use_cat' never matches.
    CHECK(out.text.find("\t1\tint cat;\n") != std::string::npos);
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    // Grammar-typed: no row's line is exactly 'use_cat'; but its line 3
    // row exists because it contains the standalone `cat` identifier.
    CHECK(out.text.find("\t3\tint use_cat() { return cat; }\n")
        != std::string::npos);
}

TEST_CASE("tool.ts.references lists call sites of a symbol")
{
    TmpDir dir;
    write_file(dir.file("use.c"),
        "int cat;\n"
        "int dog;\n"
        "int use_cat() { return cat; }\n"
        "int call_cat() { return cat(); }\n"
        "// cat() in a comment\n");

    const imza::ToolOutput out = run_script(
        "local rows, err = tool.ts.references([[" + dir.file("use.c").string()
        + "]], 'cat')\n"
          "if err then error(err) end\n"
          "for _, r in ipairs(rows) do print(r.line, r.kind, r.text) end");
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    // Only the call site on line 4 matches; the variable uses and the
    // comment are not identifier-in-call-node matches.
    CHECK(out.text == "4\tcall_expression\tint call_cat() { return cat(); }\n");
}
TEST_CASE("ts bindings fail with values on bad paths and unknown grammars")
{
    TmpDir dir;
    write_file(dir.file("a.c"), "int main() { return 0; }\n");
    write_file(dir.file("note.unknownext"), "hello\n");

    const imza::ToolOutput missing
        = run_script("local rows, err = "
                     "tool.ts.index('no-such-file.c')\nprint(rows, err)");
    CHECK(missing.text.find("no such file") != std::string::npos);

    const imza::ToolOutput nogramever
        = run_script("local rows, err = tool.ts.index([["
            + dir.file("note.unknownext").string() + "]])\nprint(rows, err)");
    CHECK(nogramever.text.find("no grammar") != std::string::npos);
}

TEST_CASE("ts bindings record dispatch log entries")
{
    TmpDir dir;
    write_file(dir.file("a.c"), "int main() { return 0; }\n");
    const std::string path     = dir.file("a.c").string();
    const imza::ToolOutput out = run_script("tool.ts.index([[" + path
        + "]])\n"
          "tool.ts.nodes([["
        + path
        + "]], 'function_definition')\n"
          "tool.ts.symbols([["
        + path
        + "]], 'main')\n"
          "tool.ts.references([["
        + path
        + "]], 'main')\n"
          "tool._lib.ts_query([["
        + path + "]], '(function_definition) @f')");
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    REQUIRE(out.dispatch_log.size() == 5);
    CHECK(out.dispatch_log[0].binding == "ts.index");
    CHECK(out.dispatch_log[1].binding == "ts.nodes");
    CHECK(out.dispatch_log[2].binding == "ts.symbols");
    CHECK(out.dispatch_log[3].binding == "ts.references");
    CHECK(out.dispatch_log[4].binding == "_lib.ts_query");
    for (const auto& entry : out.dispatch_log) {
        CHECK(entry.ok);
        CHECK(entry.target == path);
    }
}

TEST_CASE("tool._lib.ts_query runs captures and rejects invalid queries")
{
    TmpDir dir;
    write_file(dir.file("q.c"), "int main() { return 0; }\n");
    const std::string path = dir.file("q.c").string();

    const imza::ToolOutput out
        = run_script("local rows, err = tool._lib.ts_query([[" + path
            + "]], '(function_definition declarator: (function_declarator "
              "declarator: (identifier) @name))')\n"
              "if err then error(err) end\n"
              "print(#rows, rows[1].capture, rows[1].line, rows[1].text)");
    CHECK(out.text.find("1\tname\t1\tmain\n") != std::string::npos);

    const imza::ToolOutput bad
        = run_script("local rows, err = tool._lib.ts_query([[" + path
            + "]], '(function_definition')\nprint(rows, err)");
    CHECK(bad.text.find("invalid query") != std::string::npos);
}

TEST_CASE("default_tools includes lua")
{
    const auto tools = imza::default_tools();
    CHECK(imza::find_tool(tools, "lua") != nullptr);
}

TEST_CASE("filesystem bindings record a dispatch log with targets")
{
    TmpDir dir;
    write_file(dir.file("a.txt"), "one\ntwo\n");
    const imza::ToolOutput out
        = run_script("tool.read([[" + dir.file("a.txt").string()
            + "]])\n"
              "tool.list([["
            + dir.path.string()
            + "]])\n"
              "tool.grep([["
            + dir.file("a.txt").string() + "]], 'one')");
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    REQUIRE(out.dispatch_log.size() == 3);
    CHECK(out.dispatch_log[0].binding == "read");
    CHECK(out.dispatch_log[0].ok);
    CHECK(out.dispatch_log[0].target == dir.file("a.txt").string());
    CHECK(out.dispatch_log[1].binding == "list");
    CHECK(out.dispatch_log[1].ok);
    CHECK(out.dispatch_log[2].binding == "grep");
    CHECK(out.dispatch_log[2].ok);
}

TEST_CASE("sh binding logs commands with exit status")
{
    TmpDir dir;
    ShellFixture fx;
    const imza::ToolOutput out = run_script(
        "tool.shell('echo hi')\ntool.shell('false')\nlocal _, err = "
        "tool.shell('echo a && echo b')\nprint(err ~= nil)",
        fx.host());
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    REQUIRE(out.dispatch_log.size() == 2);
    CHECK(out.dispatch_log[0].binding == "shell");
    CHECK(out.dispatch_log[0].target == "echo hi");
    CHECK(out.dispatch_log[0].ok);
    CHECK_FALSE(out.dispatch_log[1].ok);
}

TEST_CASE("a script killed at the deadline keeps its partial log")
{
    TmpDir dir;
    write_file(dir.file("a.txt"), "x\n");
    const imza::ToolOutput out
        = run_script("tool.read([[" + dir.file("a.txt").string()
            + "]])\n"
              "while true do end");
    CHECK(out.kind == imza::ToolOutput::Kind::ERROR);
    REQUIRE(out.dispatch_log.size() == 1);
    CHECK(out.dispatch_log[0].binding == "read");
    CHECK(out.dispatch_log[0].ok);
}

std::string read_all(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string(
        (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

TEST_CASE("tool.file.insert inserts before a line and appends with nil")
{
    TmpDir dir;
    write_file(dir.file("a.txt"), "one\ntwo\nthree\n");
    const std::string path     = dir.file("a.txt").string();
    const imza::ToolOutput out = run_script("assert(tool.file.insert([[" + path
        + "]], 'inserted', 2))\n"
          "assert(tool.file.insert([["
        + path + "]], 'tail'))");
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(read_all(dir.file("a.txt")) == "one\ninserted\ntwo\nthree\ntail\n");
    REQUIRE(out.diffs.size() == 1);
    CHECK(out.diffs[0].file == path);
}

TEST_CASE("tool.file.insert rejects out-of-range lines and missing files")
{
    TmpDir dir;
    write_file(dir.file("a.txt"), "one\ntwo\n");
    const std::string path = dir.file("a.txt").string();
    const imza::ToolOutput out
        = run_script("local ok, err = tool.file.insert([[" + path
            + "]], 'x', 10)\n"
              "print(ok, err)\n"
              "local ok2, err2 = tool.file.insert([["
            + dir.file("missing.txt").string()
            + "]], 'x')\n"
              "print(ok2, err2)");
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text.find("exceeds file length") != std::string::npos);
    CHECK(out.text.find("no such file") != std::string::npos);
    CHECK(out.diffs.empty());
}

TEST_CASE("tool.file.edit replaces first occurrence by default")
{
    TmpDir dir;
    write_file(dir.file("a.txt"), "foo bar foo baz foo\n");
    const std::string path = dir.file("a.txt").string();
    const imza::ToolOutput out
        = run_script("assert(tool.file.edit([[" + path + "]], 'foo', 'qux'))");
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(read_all(dir.file("a.txt")) == "qux bar foo baz foo\n");
}

TEST_CASE("tool.file.edit count=0 replaces all occurrences")
{
    TmpDir dir;
    write_file(dir.file("a.txt"), "foo bar foo baz foo\n");
    const std::string path     = dir.file("a.txt").string();
    const imza::ToolOutput out = run_script(
        "assert(tool.file.edit([[" + path + "]], 'foo', 'qux', 0))");
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(read_all(dir.file("a.txt")) == "qux bar qux baz qux\n");
}

TEST_CASE("tool.file.edit errors on missing match and empty old")
{
    TmpDir dir;
    write_file(dir.file("a.txt"), "hello\n");
    const std::string path = dir.file("a.txt").string();
    const imza::ToolOutput out
        = run_script("local ok, err = tool.file.edit([[" + path
            + "]], 'absent', 'x')\n"
              "print(ok, err)\n"
              "local ok2, err2 = tool.file.edit([["
            + path
            + "]], '', 'x')\n"
              "print(ok2, err2)");
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text.find("not found") != std::string::npos);
    CHECK(out.text.find("non-empty") != std::string::npos);
    CHECK(read_all(dir.file("a.txt")) == "hello\n");
}

TEST_CASE("tool.file.write creates and rewrites files")
{
    TmpDir dir;
    const std::string path     = dir.file("new.txt").string();
    const imza::ToolOutput out = run_script("assert(tool.file.write([[" + path
        + "]], 'v1\\n'))\n"
          "assert(tool.file.write([["
        + path + "]], 'v2\\n'))");
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(read_all(dir.file("new.txt")) == "v2\n");
    // One net diff per file: original (empty) to latest.
    REQUIRE(out.diffs.size() == 1);
    CHECK(out.diffs[0].file == path);
}

TEST_CASE("lua file mutations collapse into one net diff per file")
{
    TmpDir dir;
    write_file(dir.file("a.txt"), "one\ntwo\nthree\n");
    write_file(dir.file("b.txt"), "alpha\n");
    const std::string a        = dir.file("a.txt").string();
    const std::string b        = dir.file("b.txt").string();
    const imza::ToolOutput out = run_script("assert(tool.file.edit([[" + a
        + "]], 'two', 'TWO'))\n"
          "assert(tool.file.insert([["
        + a
        + "]], 'zero', 1))\n"
          "assert(tool.file.edit([["
        + b + "]], 'alpha', 'ALPHA'))");
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    REQUIRE(out.diffs.size() == 2);
    CHECK(out.diffs[0].file == a);
    CHECK(out.diffs[1].file == b);
    // Net diff for a.txt shows original (one,two,three) against the final
    // content (zero,one,TWO,three) in a single span.
    std::size_t adds = 0;
    for (const imza::DiffRow& row : out.diffs[0].rows) {
        if (row.kind == imza::DiffRow::Kind::ADD) {
            ++adds;
        }
    }
    CHECK(adds == 3);
}

TEST_CASE("lua file diffs survive a mid-script error")
{
    TmpDir dir;
    write_file(dir.file("a.txt"), "one\n");
    const std::string a        = dir.file("a.txt").string();
    const imza::ToolOutput out = run_script("assert(tool.file.edit([[" + a
        + "]], 'one', 'ONE'))\n"
          "error('boom')");
    CHECK(out.kind == imza::ToolOutput::Kind::ERROR);
    REQUIRE(out.diffs.size() == 1);
    CHECK(out.diffs[0].file == a);
    CHECK(read_all(dir.file("a.txt")) == "ONE\n");
}

TEST_CASE("tool.file bindings record dispatch log entries")
{
    TmpDir dir;
    write_file(dir.file("a.txt"), "one\n");
    const std::string a        = dir.file("a.txt").string();
    const imza::ToolOutput out = run_script("tool.file.edit([[" + a
        + "]], 'one', 'ONE')\n"
          "tool.file.insert([["
        + a
        + "]], 'x')\n"
          "tool.file.write([["
        + dir.file("b.txt").string() + "]], 'y\\n')");
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    REQUIRE(out.dispatch_log.size() == 3);
    CHECK(out.dispatch_log[0].binding == "file.edit");
    CHECK(out.dispatch_log[1].binding == "file.insert");
    CHECK(out.dispatch_log[2].binding == "file.write");
    for (const auto& entry : out.dispatch_log) {
        CHECK(entry.ok);
    }
}

TEST_CASE("tool.file mutations reject in Plan mode")
{
    TmpDir dir;
    write_file(dir.file("a.txt"), "one\n");
    imza::LuaHost host;
    auto system    = std::make_shared<imza::SystemEnvironment>();
    auto workspace = std::make_shared<imza::WorkspaceEnvironment>();
    workspace->working_directory = dir.path;
    workspace->project_root      = dir.path;
    imza::PermissionStore store;
    host.permission_context = [&] {
        return imza::PermissionContext { system, workspace, store.snapshot(),
            imza::SessionMode::PLAN };
    };
    const std::string a        = dir.file("a.txt").string();
    const imza::ToolOutput out = run_script("local ok, err = tool.file.edit([["
            + a + "]], 'one', 'ONE')\nprint(ok, err)",
        std::move(host));
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text.find("permission denied") != std::string::npos);
    CHECK(read_all(dir.file("a.txt")) == "one\n");
}

TEST_CASE("tool.file mutations auto-accept in trusted Build mode")
{
    TmpDir dir;
    write_file(dir.file("a.txt"), "one\n");
    imza::LuaHost host;
    auto system    = std::make_shared<imza::SystemEnvironment>();
    auto workspace = std::make_shared<imza::WorkspaceEnvironment>();
    workspace->working_directory = dir.path;
    workspace->project_root      = dir.path;
    imza::PermissionStore store;
    host.permission_context = [&] {
        return imza::PermissionContext { system, workspace, store.snapshot(),
            imza::SessionMode::BUILD };
    };
    const std::string a        = dir.file("a.txt").string();
    const imza::ToolOutput out = run_script(
        "assert(tool.file.edit([[" + a + "]], 'one', 'ONE'))", std::move(host));
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(read_all(dir.file("a.txt")) == "ONE\n");
}

TEST_CASE("tool.file mutations outside the workspace ask and fail closed "
          "unattended")
{
    TmpDir dir;
    TmpDir outside;
    write_file(outside.file("a.txt"), "one\n");
    imza::LuaHost host;
    auto system    = std::make_shared<imza::SystemEnvironment>();
    auto workspace = std::make_shared<imza::WorkspaceEnvironment>();
    workspace->working_directory = dir.path;
    workspace->project_root      = dir.path;
    imza::PermissionStore store;
    host.permission_context = [&] {
        return imza::PermissionContext { system, workspace, store.snapshot(),
            imza::SessionMode::BUILD };
    };
    // No ask callback: ASK verdicts must fail closed.
    const std::string a        = outside.file("a.txt").string();
    const imza::ToolOutput out = run_script("local ok, err = tool.file.edit([["
            + a + "]], 'one', 'ONE')\nprint(ok, err)",
        std::move(host));
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text.find("permission denied") != std::string::npos);
    CHECK(read_all(outside.file("a.txt")) == "one\n");
}

TEST_CASE("tool.file mutations proceed after attended approval")
{
    TmpDir dir;
    TmpDir outside;
    write_file(outside.file("a.txt"), "one\n");
    imza::LuaHost host;
    auto system    = std::make_shared<imza::SystemEnvironment>();
    auto workspace = std::make_shared<imza::WorkspaceEnvironment>();
    workspace->working_directory = dir.path;
    workspace->project_root      = dir.path;
    imza::PermissionStore store;
    host.permission_context = [&] {
        return imza::PermissionContext { system, workspace, store.snapshot(),
            imza::SessionMode::BUILD };
    };
    host.ask = [&](imza::ModalPayload payload) {
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
    const std::string a        = outside.file("a.txt").string();
    const imza::ToolOutput out = run_script(
        "assert(tool.file.edit([[" + a + "]], 'one', 'ONE'))", std::move(host));
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(read_all(outside.file("a.txt")) == "ONE\n");
}
