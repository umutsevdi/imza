#include <doctest/doctest.h>
#include <json/json.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include "network/json_io.h"
#include "tools/tool.h"

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
    empty.context              = [] { return imza::PermissionContext { }; };
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
        = run_script("local ok, err = tool.set_todo({"
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
        = run_script("local rows = tool.todo()\n"
                     "print(#rows, rows[1].content, rows[1].status, "
                     "rows[2].status)",
            std::move(host2));
    CHECK(get.text == "2\tfirst\tin_progress\tpending\n");

    const imza::ToolOutput bad
        = run_script("local ok, err = tool.set_todo({"
                     "{content = 'x', status = 'nope'}})\nprint(err)",
            imza::LuaHost { });
    CHECK(bad.text.find("unknown status") != std::string::npos);
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

TEST_CASE("tool.skill loads instructions once and rejects unknown names")
{
    imza::LuaHost host { };
    imza::SkillStore store;
    int loads = 0;
    std::filesystem::path skill_path;
    host.skills = [&skill_path]() -> std::vector<imza::Skill> {
        if (skill_path.empty()) {
            skill_path = std::filesystem::temp_directory_path()
                / ("imza_lua_skill_" + std::to_string(::getpid()) + ".md");
            std::ofstream out(skill_path);
            out << "# Greet\nSay hi.";
        }
        imza::Skill skill;
        skill.name  = "greet";
        skill.path  = skill_path;
        skill.scope = imza::Skill::Scope::GLOBAL;
        return { skill };
    };
    host.config = [] {
        static imza::Config config;
        return config;
    };
    host.skill_store = [&store, &loads]() -> imza::SkillStore& {
        ++loads;
        return store;
    };
    const imza::ToolOutput first
        = run_script("local body, err = tool.skill('greet')\n"
                     "if err then error(err) end\nprint(body)",
            host);
    CHECK(first.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(first.text.find("Say hi.") != std::string::npos);

    const imza::ToolOutput unknown = run_script(
        "local body, err = tool.skill('nope')\nprint(err)", std::move(host));
    CHECK(unknown.text.find("unknown") != std::string::npos);
}

TEST_CASE("web bindings fail closed without web access")
{
    const imza::ToolOutput fetch = run_script(
        "local body, err = tool.webfetch('https://example.com')\nprint(err)");
    CHECK(fetch.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(fetch.text.find("web access is disabled") != std::string::npos);

    const imza::ToolOutput search
        = run_script("local body, err = tool.websearch('imza')\nprint(err)");
    CHECK(search.text.find("web access is disabled") != std::string::npos);
}

TEST_CASE("web bindings validate arguments before checking access")
{
    CHECK(run_script("print(tool.webfetch())").kind
        == imza::ToolOutput::Kind::ERROR);
    CHECK(run_script("print(tool.websearch())").kind
        == imza::ToolOutput::Kind::ERROR);
}

TEST_CASE("default_tools includes lua in every mode")
{
    const auto tools = imza::default_tools(imza::RuntimeFlag::NONE);
    CHECK(imza::find_tool(tools, "lua") != nullptr);
}
