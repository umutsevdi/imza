#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <optional>
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
              / ("imza_tools_test_" + std::to_string(next_dir_id())))
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

imza::ToolOutput run(const imza::Tool& tool, std::string_view path)
{
    return tool.run(imza::parse_json(
        std::string(R"({"path":")") + std::string(path) + "\"}"));
}

imza::ToolOutput run_window(
    const imza::Tool& tool, std::string_view path, int begin, int end)
{
    const std::string args = std::string(R"({"path":")") + std::string(path)
        + "\",\"line_begin\":" + std::to_string(begin)
        + ",\"line_end\":" + std::to_string(end) + "}";
    return tool.run(imza::parse_json(args));
}

imza::ToolOutput run_find(const imza::Tool& tool, std::string pattern,
    const std::optional<fs::path>& path = std::nullopt)
{
    Json::Value args(Json::objectValue);
    args["pattern"] = std::move(pattern);
    if (path) {
        args["path"] = path->string();
    }
    return tool.run(args);
}

} // namespace

TEST_CASE("read returns the full file")
{
    TmpDir tmp;
    write_file(tmp.file("notes.txt"), "alpha\nbeta\ngamma\n");
    const auto tool = imza::make_read_tool();

    const auto out = run(tool, tmp.file("notes.txt").string());
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text == "alpha\nbeta\ngamma");
}

TEST_CASE("read returns the requested window")
{
    TmpDir tmp;
    write_file(tmp.file("lines.txt"), "one\ntwo\nthree\nfour\nfive\n");
    const auto tool = imza::make_read_tool();

    const auto out = run_window(tool, tmp.file("lines.txt").string(), 2, 4);
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text == "two\nthree\nfour");
}

TEST_CASE("read reports range and path errors")
{
    TmpDir tmp;
    write_file(tmp.file("small.txt"), "a\nb\n");
    const auto tool     = imza::make_read_tool();
    const std::string p = tmp.file("small.txt").string();

    const auto beyond = run_window(tool, p, 5, 9);
    CHECK(beyond.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(beyond.text.find("exceeds file length 2") != std::string::npos);

    const auto missing = run(tool, tmp.file("nope.txt").string());
    CHECK(missing.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(missing.text.find("no such file") != std::string::npos);

    const auto dir = run(tool, tmp.path.string());
    CHECK(dir.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(dir.text.find("not a file") != std::string::npos);
}

TEST_CASE("read clamps line_end to the file length")
{
    TmpDir tmp;
    write_file(tmp.file("small.txt"), "a\nb\nc\n");
    const auto tool = imza::make_read_tool();

    const auto out = run_window(tool, tmp.file("small.txt").string(), 2, 1000);
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text == "b\nc");
}

TEST_CASE("read rejects binary files and reports empty files")
{
    TmpDir tmp;
    write_file(tmp.file("bin.dat"), std::string("a\0b", 3));
    const auto tool = imza::make_read_tool();

    const auto bin = run(tool, tmp.file("bin.dat").string());
    CHECK(bin.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(bin.text.find("binary") != std::string::npos);

    write_file(tmp.file("empty.txt"), "");
    const auto empty = run(tool, tmp.file("empty.txt").string());
    CHECK(empty.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(empty.text == "(empty file)");
}

TEST_CASE("read truncates unbounded reads of long files")
{
    TmpDir tmp;
    std::string body;
    for (int i = 1; i <= 2500; ++i) {
        body += "line" + std::to_string(i) + "\n";
    }
    write_file(tmp.file("long.txt"), body);
    const auto tool = imza::make_read_tool();

    const auto out = run(tool, tmp.file("long.txt").string());
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text.find("[truncated: showing lines 1-2000 of 2500]")
        != std::string::npos);
    CHECK(out.text.find("line2000\n") != std::string::npos);
    CHECK(out.text.find("\nline2001") == std::string::npos);

    const auto window
        = run_window(tool, tmp.file("long.txt").string(), 2400, 2500);
    CHECK(window.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(window.text.find("truncated") == std::string::npos);
    CHECK(window.text.find("line2500") != std::string::npos);
}

TEST_CASE("list returns sorted entries with directory markers")
{
    TmpDir tmp;
    write_file(tmp.file("beta.txt"), "b");
    write_file(tmp.file("alpha.txt"), "a");
    fs::create_directory(tmp.file("zed"));
    write_file(tmp.file(".hidden"), "h");
    const auto tool = imza::make_list_tool();

    const auto out = run(tool, tmp.path.string());
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text
        == ".hidden    0 KB\nalpha.txt  0 KB\nbeta.txt   0 KB\nzed/       —");
}

TEST_CASE("list defaults to the current directory")
{
    TmpDir tmp;
    write_file(tmp.file("marker.txt"), "m");
    const auto tool = imza::make_list_tool();

    const fs::path cwd = fs::current_path();
    fs::current_path(tmp.path);
    const auto out = tool.run(imza::parse_json("{}"));
    fs::current_path(cwd);

    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text.find("marker.txt") != std::string::npos);
    CHECK(out.text.find("./") == std::string::npos);
}

TEST_CASE("list rejects non-directories and reports empty output")
{
    TmpDir tmp;
    write_file(tmp.file("file.txt"), "x");
    const auto tool = imza::make_list_tool();

    const auto not_dir = run(tool, tmp.file("file.txt").string());
    CHECK(not_dir.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(not_dir.text.find("not a directory") != std::string::npos);

    const auto missing = run(tool, tmp.file("gone").string());
    CHECK(missing.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(missing.text.find("no such directory") != std::string::npos);

    fs::create_directory(tmp.file("emptydir"));
    const auto empty = run(tool, tmp.file("emptydir").string());
    CHECK(empty.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(empty.text.empty());
}

TEST_CASE("find fallback searches recursively without opening a viewer")
{
    TmpDir tmp;
    fs::create_directory(tmp.file("nested"));
    write_file(tmp.file("nested/match.txt"), "alpha\nneedle 42\nomega\n");
    write_file(tmp.file("other.txt"), "nothing here\n");
    const auto tool = imza::make_find_tool(false);

    const auto out = run_find(tool, R"(needle [0-9]+)", tmp.path);
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text.find("match.txt:2:needle 42") != std::string::npos);
    CHECK_FALSE(out.viewer.has_value());
}

TEST_CASE("find handles no matches and validates arguments")
{
    TmpDir tmp;
    write_file(tmp.file("file.txt"), "content\n");
    const auto tool = imza::make_find_tool(false);

    const auto none = run_find(tool, "absent", tmp.path);
    REQUIRE(none.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(none.text.empty());
    CHECK_FALSE(none.viewer.has_value());

    CHECK(
        tool.run(imza::parse_json("{}")).kind == imza::ToolOutput::Kind::ERROR);
    CHECK(tool.run(imza::parse_json(R"({"pattern":""})")).kind
        == imza::ToolOutput::Kind::ERROR);
    CHECK(tool.run(imza::parse_json(R"({"pattern":"x","path":3})")).kind
        == imza::ToolOutput::Kind::ERROR);
}

TEST_CASE("find defaults to the current directory")
{
    TmpDir tmp;
    write_file(tmp.file("default.txt"), "default-find-marker\n");
    const auto tool      = imza::make_find_tool(false);
    const fs::path saved = fs::current_path();
    fs::current_path(tmp.path);
    const auto out = run_find(tool, "default-find-marker");
    fs::current_path(saved);

    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text.find("default-find-marker") != std::string::npos);
}

TEST_CASE("find passes shell metacharacters as regex text")
{
    TmpDir tmp;
    write_file(tmp.file("file.txt"), "literal;echo value\n");
    const auto tool = imza::make_find_tool(false);

    const auto out = run_find(tool, R"(literal;echo)", tmp.path);
    REQUIRE(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text.find("literal;echo value") != std::string::npos);
}

TEST_CASE("builtin tools expose the current tool set")
{
    const auto tools = imza::default_tools();
    REQUIRE(tools.size() == 12);

    const auto* read = find_tool(tools, "read");
    REQUIRE(read != nullptr);
    CHECK(read->spec.parameters["properties"].isMember("path"));

    const auto* list = find_tool(tools, "list");
    REQUIRE(list != nullptr);
    CHECK(list->spec.parameters["properties"].isMember("path"));

    const auto* find = find_tool(tools, "find");
    REQUIRE(find != nullptr);
    CHECK(find->spec.parameters["properties"].isMember("pattern"));
    CHECK(find->spec.parameters["properties"].isMember("path"));

    const auto* ask = find_tool(tools, "ask");
    REQUIRE(ask != nullptr);
    CHECK(ask->spec.parameters["properties"].isMember("questions"));

    const auto* skill = find_tool(tools, "skill");
    REQUIRE(skill != nullptr);
    CHECK(skill->spec.parameters["properties"].isMember("name"));

    REQUIRE(find_tool(tools, "shell") != nullptr);
    REQUIRE(find_tool(tools, "todo") != nullptr);
    const auto* subagent = find_tool(tools, "subagent");
    REQUIRE(subagent != nullptr);
    CHECK(subagent->spec.parameters["properties"].isMember("tasks"));
    REQUIRE(find_tool(tools, "edit") != nullptr);
    REQUIRE(find_tool(tools, "write") != nullptr);
    REQUIRE(find_tool(tools, "webfetch") != nullptr);
    REQUIRE(find_tool(tools, "websearch") != nullptr);
    CHECK(tool_specs(tools).size() == 12);
}

TEST_CASE("runtime flags independently filter the tool roster")
{
    int flags = imza::RuntimeFlag::WEB;
    const auto web_only
        = imza::default_tools(static_cast<imza::RuntimeFlag>(flags));

    CHECK(imza::find_tool(web_only, "ask") == nullptr);
    CHECK(imza::find_tool(web_only, "shell") == nullptr);
    CHECK(imza::find_tool(web_only, "webfetch") != nullptr);
    CHECK(imza::find_tool(web_only, "websearch") != nullptr);

    flags = imza::RuntimeFlag::SHELL | imza::RuntimeFlag::ATTENDED
        | imza::RuntimeFlag::SKIP_PERMISSIONS;
    const auto attended_shell
        = imza::default_tools(static_cast<imza::RuntimeFlag>(flags));

    CHECK(imza::find_tool(attended_shell, "ask") != nullptr);
    CHECK(imza::find_tool(attended_shell, "shell") != nullptr);
    CHECK(imza::find_tool(attended_shell, "webfetch") == nullptr);
    CHECK(imza::find_tool(attended_shell, "websearch") == nullptr);

    const auto tools = imza::default_tools(imza::RuntimeFlag::NONE);

    CHECK(imza::find_tool(tools, "ask") == nullptr);
    CHECK(imza::find_tool(tools, "shell") == nullptr);
    CHECK(imza::find_tool(tools, "webfetch") == nullptr);
    CHECK(imza::find_tool(tools, "websearch") == nullptr);
    CHECK(imza::find_tool(tools, "read") != nullptr);
    CHECK(imza::find_tool(tools, "find") != nullptr);
    CHECK(imza::find_tool(tools, "write") != nullptr);

    const auto disabled = imza::dispatch_tool(
        tools, { "shell", R"({"command":"echo unavailable"})", "", "" });
    CHECK(disabled.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(disabled.text == "unknown tool: shell");
}

TEST_CASE("shell tool runs a command and reports the exit code")
{
    const auto tool = imza::make_shell_tool();
    CHECK(tool.spec.parameters["properties"].isMember("command"));
    CHECK(tool.spec.parameters["properties"].isMember("timeout"));

    const auto out
        = tool.run(imza::parse_json(R"({"command":"echo hi-from-shell"})"));
    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text.find("hi-from-shell") != std::string::npos);
    CHECK_FALSE(out.text.starts_with("echo hi-from-shell"));
    CHECK(out.text.find("exit code") == std::string::npos);
    REQUIRE(out.shell_status.has_value());
    const auto* status = std::get_if<imza::ShellExit>(&*out.shell_status);
    REQUIRE(status != nullptr);
    CHECK(status->code == 0);
}

TEST_CASE("shell tool rejects a missing command and honors timeout clamp")
{
    const auto tool = imza::make_shell_tool();

    const auto empty = tool.run(imza::parse_json(R"({"command":""})"));
    CHECK(empty.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(empty.text.find("non-empty") != std::string::npos);

    const auto bad_timeout
        = tool.run(imza::parse_json(R"({"command":"true","timeout":0})"));
    CHECK(bad_timeout.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(bad_timeout.text.find("timeout") != std::string::npos);
}

TEST_CASE("shell tool carries non-zero exit status separately from output")
{
    const auto tool = imza::make_shell_tool();
    const auto out  = tool.run(imza::parse_json(R"({"command":"false"})"));

    CHECK(out.kind == imza::ToolOutput::Kind::OUTPUT);
    CHECK(out.text.empty());
    REQUIRE(out.shell_status.has_value());
    const auto* status = std::get_if<imza::ShellExit>(&*out.shell_status);
    REQUIRE(status != nullptr);
    CHECK(status->code != 0);
}
