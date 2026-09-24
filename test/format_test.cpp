#include <string>

#include <doctest/doctest.h>
#include <json/json.h>

#include "conversation/format.h"
#include "network/json_io.h"
#include "ui/tool_format.h"

TEST_CASE("question_form_markdown renders prompt and options")
{
    imza::QuestionForm form { { "model?", { "gpt-4o", "claude" }, false,
        false } };
    const std::string md = imza::question_form_markdown(form);
    CHECK(md.find("Question: \"model?\"") != std::string::npos);
    CHECK(md.find("- gpt-4o") != std::string::npos);
    CHECK(md.find("- claude") != std::string::npos);
}

TEST_CASE("modal_answer_markdown renders selected then free text")
{
    imza::ModalAnswer ans { { { { "Option 3" }, "extra note" } } };
    const std::string md = imza::modal_answer_markdown(ans);
    CHECK(md.find("User answered:") == 0);
    CHECK(md.find("> Option 3\n> extra note") != std::string::npos);
}

TEST_CASE("lua dispatch log formats as counts and grouped summary")
{
    imza::ToolCall call;
    call.name   = "lua";
    call.result = imza::ToolCall::Result { imza::ToolCall::Result::Kind::OUTPUT,
        "done\n" };
    call.result->dispatch_log = {
        { "read", "a.cpp", true },
        { "list", "src", true },
        { "read", "b.cpp", true },
        { "list", "test", false },
        { "list", "include", true },
    };
    CHECK(imza::lua_dispatch_counts(call) == "5 tools (1 failed)");
    CHECK(imza::lua_dispatch_summary(call) == "2 read · 3 list (1 failed)");

    call.result->dispatch_log = { { "read", "a.cpp", true } };
    CHECK(imza::lua_dispatch_counts(call) == "1 tool");
}

TEST_CASE("lua viewer report fences script and output safely")
{
    imza::ToolCall call;
    call.name   = "lua";
    call.args   = R"json({"script":"print('```')"})json";
    call.result = imza::ToolCall::Result { imza::ToolCall::Result::Kind::OUTPUT,
        "ok\n" };
    const std::string report = imza::lua_viewer_content(call);
    CHECK(report.find("````lua\nprint('```')\n````") != std::string::npos);
    CHECK(report.find("````txt\nok\n\n````") != std::string::npos);
}
TEST_CASE("lua viewer appends a rendered return-value block")
{
    imza::ToolCall call;
    call.name   = "lua";
    call.args   = R"json({"script":"return {a = 1}"})json";
    call.result = imza::ToolCall::Result { imza::ToolCall::Result::Kind::OUTPUT,
        "log\n" };
    call.result->return_value = imza::parse_json(R"json({"a":1})json");
    const std::string report  = imza::lua_viewer_content(call);
    CHECK(report.find("```lua\nreturn {a = 1}\n```") != std::string::npos);
    CHECK(report.find("```txt\nlog\n\n```") != std::string::npos);
    // JSON returns are fenced with the json language for highlighting.
    CHECK(report.find("```json\n{\n  \"a\" : 1\n}\n```") != std::string::npos);

    // The return-value block is the last section.
    const std::size_t json_at = report.find("```json");
    const std::size_t txt_at  = report.find("```txt");
    CHECK(json_at > txt_at);
}

TEST_CASE("lua viewer appends a quoted error block after the return value")
{
    imza::ToolCall call;
    call.name   = "lua";
    call.args   = R"json({"script":"return 1"})json";
    call.result = imza::ToolCall::Result { imza::ToolCall::Result::Kind::ERROR,
        "script:1: boom\n" };
    call.result->return_value = imza::parse_json(R"json({"a":1})json");
    const std::string report  = imza::lua_viewer_content(call);
    CHECK(report.find("**Error**") != std::string::npos);
    CHECK(report.find("> [!ERROR]") != std::string::npos);
    CHECK(report.find("> script:1: boom") != std::string::npos);
    // The error text is carried only by the quoted block, not echoed as
    // a plain output section.
    CHECK(report.find("```txt\nscript:1: boom") == std::string::npos);
    // The error follows the return-value block, ahead of any diffs.
    const std::size_t err_at  = report.find("**Error**");
    const std::size_t json_at = report.find("```json");
    CHECK(err_at > json_at);
}

TEST_CASE("lua viewer omits the return-value block when there is none")
{
    imza::ToolCall call;
    call.name   = "lua";
    call.args   = R"json({"script":"print('x')"})json";
    call.result = imza::ToolCall::Result { imza::ToolCall::Result::Kind::OUTPUT,
        "x\n" };
    const std::string report = imza::lua_viewer_content(call);
    CHECK(report.find("```json") == std::string::npos);
    CHECK(report.find("|path|") == std::string::npos);
}

TEST_CASE("lua viewer skips the output block when nothing was printed")
{
    imza::ToolCall call;
    call.name = "lua";
    call.args = R"json({"script":"return 42"})json";
    call.result
        = imza::ToolCall::Result { imza::ToolCall::Result::Kind::OUTPUT, "" };
    call.result->return_value = imza::parse_json("42");
    const std::string report  = imza::lua_viewer_content(call);
    CHECK(report.find("```txt") == std::string::npos);
    CHECK(report.find("(no output)") == std::string::npos);
    CHECK(report.find("\n42") != std::string::npos);
}

TEST_CASE("lua viewer puts string returns in a code block")
{
    imza::ToolCall call;
    call.name = "lua";
    call.args = R"json({"script":"return '## not a heading'"})json";
    call.result
        = imza::ToolCall::Result { imza::ToolCall::Result::Kind::OUTPUT, "" };
    call.result->return_value
        = imza::parse_json(R"json("## not a heading")json");
    const std::string report = imza::lua_viewer_content(call);
    CHECK(report.find("```txt\n## not a heading\n```") != std::string::npos);
}

TEST_CASE("lua viewer renders table returns as markdown, not JSON")
{
    imza::ToolCall call;
    call.name = "lua";
    call.args = R"json({"script":"return tool.list()"})json";
    call.result
        = imza::ToolCall::Result { imza::ToolCall::Result::Kind::OUTPUT, "" };
    call.result->return_value = imza::parse_json(
        R"json([{"path":"src/a.cpp","type":"file","size":"1.2 KB"}])json");
    const std::string report = imza::lua_viewer_content(call);
    // The table must appear bare so the markdown renderer renders it.
    CHECK(report.find("\n|path|size|type|\n") != std::string::npos);
    CHECK(report.find("```json") == std::string::npos);
}

TEST_CASE("format_lua_result appends returned values to the transcript")
{
    CHECK(imza::format_lua_result("done\n", std::nullopt) == "done\n");

    // Non-array objects fall back to fenced JSON.
    const Json::Value value = imza::parse_json(R"json({"a":1})json");
    const std::string with  = imza::format_lua_result("done\n", value);
    CHECK(with.find("done\n") == 0);
    CHECK(with.find("```json\n{\n  \"a\" : 1\n}\n```") != std::string::npos);

    // No printed output: the JSON block starts the result.
    const std::string only = imza::format_lua_result("", value);
    CHECK(only.find("```json") == 0);

    // A returned JSON fallback containing backticks gets a larger fence.
    const Json::Value tricky = imza::parse_json(R"json({"a":"```"})json");
    const std::string fenced = imza::format_lua_result("", tricky);
    CHECK(fenced.find("````json") != std::string::npos);
}

TEST_CASE("format_lua_result prints scalars and scalar lists directly")
{
    const Json::Value number = imza::parse_json("42");
    CHECK(imza::format_lua_result("", number) == "42");

    // Strings are free-form: fenced so markdown does not eat them.
    const Json::Value text = imza::parse_json(R"json("hello world")json");
    CHECK(imza::format_lua_result("log\n", text)
        == "log\n\n```\nhello world\n```");

    const Json::Value flag = imza::parse_json("true");
    CHECK(imza::format_lua_result("", flag) == "true");

    const Json::Value list = imza::parse_json(R"json([1,2,3])json");
    CHECK(imza::format_lua_result("", list) == "1\n2\n3");

    const Json::Value mixed
        = imza::parse_json(R"json([{"path":"src"},"done"])json");
    const std::string mixed_result = imza::format_lua_result("", mixed);
    CHECK(mixed_result.find("```json\n[") == 0);
    CHECK(mixed_result.find("\"done\"") != std::string::npos);

    // A scalar string that merely starts with "[" must not be JSON-fenced.
    const Json::Value bracketed = imza::parse_json(R"json("[a]")json");
    CHECK(imza::format_lua_result("", bracketed) == "```\n[a]\n```");
}

TEST_CASE("format_lua_return renders record lists as markdown tables")
{
    const Json::Value records = imza::parse_json(
        R"json([
            {"path":"src/a.cpp","type":"file","size":"1.2 KB"},
            {"path":"src","type":"dir"},
            {"path":"note|a\nb","type":"file","size":"3 KB"}
        ])json");
    const std::string table = imza::format_lua_return(records);
    // jsoncpp stores object keys sorted, so columns follow that order.
    const std::vector<std::string> lines
        = { "|path|size|type|", "|---|---|---|", "|src/a.cpp|1.2 KB|file|",
              "|src||dir|", "|note\\|a<br>b|3 KB|file|" };
    std::size_t at = 0;
    for (const std::string& line : lines) {
        const std::size_t found = table.find(line, at);
        REQUIRE(found != std::string::npos);
        at = found + line.size();
    }

    // Nested values demote the list to JSON.
    const Json::Value nested = imza::parse_json(R"json([{"a":[1]}])json");
    CHECK(imza::format_lua_return(nested).find("|") == std::string::npos);
}

TEST_CASE("modal_answer_markdown renders Q/A pairs with prompt")
{
    imza::ModalAnswer ans;
    ans.cards.push_back(
        imza::QuestionAnswer { { "PostgreSQL" }, "", "storage backend?" });
    ans.cards.push_back(
        imza::QuestionAnswer { { "Auth", "Billing" }, "", "features?" });
    ans.cards.push_back(
        imza::QuestionAnswer { { }, "my own region", "region?" });
    ans.cards.push_back(imza::QuestionAnswer { { }, "", "anything else?" });
    const std::string md = imza::modal_answer_markdown(ans);
    CHECK(md.find("**storage backend?**") != std::string::npos);
    CHECK(md.find("PostgreSQL") != std::string::npos);
    CHECK(md.find("Auth, Billing") != std::string::npos);
    CHECK(md.find("my own region") != std::string::npos);
    CHECK(md.find("anything else?**") != std::string::npos);
    CHECK(md.find("-") != std::string::npos);
}

TEST_CASE("tool_args_summary formats object and non-object args")
{
    CHECK(imza::tool_args_summary(R"({"path":"notes.txt","n":3})")
        == "n=3 path=notes.txt");
    CHECK(imza::tool_args_summary(R"({"flag":true})") == "flag=true");
    CHECK(imza::tool_args_summary(R"({"path":null})") == "path=null");
    CHECK(imza::tool_args_summary("ls -la") == "ls -la");
    CHECK(imza::tool_args_summary("{}") == "{}");
}

TEST_CASE("tool_call_head special-cases roster tools, args otherwise")
{
    imza::ToolCall skill { 1, "", "skill",
        R"({"name":"code-review","scope":"project"})", { } };
    CHECK(imza::tool_call_head(skill) == "Load Skill code-review");
    CHECK(imza::tool_header_args(skill) == "code-review");

    imza::ToolCall subagent { 1, "", "subagent",
        R"({"tasks":[{"mode":"research","prompt":"Inspect parser behavior"},{"mode":"build","prompt":"Implement the fix"}]})",
        { } };
    CHECK(imza::tool_call_head(subagent) == "Subagent");
    CHECK(imza::tool_header_args(subagent) == "1 research, 1 builder");

    // Names the roster no longer offers (pre-0.4 sessions) fall through to
    // the generic display-name + argument summary.
    imza::ToolCall read { 1, "", "read",
        R"({"path":"src/a.cpp","line_begin":1})", { } };
    CHECK(imza::tool_call_head(read) == "Read line_begin=1 path=src/a.cpp");

    imza::ToolCall other { 1, "", "bash", "git status", { } };
    CHECK(imza::tool_call_head(other) == "Bash git status");
}

TEST_CASE("lua headers summarize bindings and never echo the script")
{
    const std::string script = R"json({"script":"local x = 1\nprint(x)"})json";

    imza::ToolCall pending { 1, "", "lua", script, { } };
    CHECK(imza::tool_call_head(pending) == "Lua execution");
    CHECK(imza::tool_header_args(pending).empty());

    imza::ToolCall done { 1, "", "lua", script, { } };
    done.result = imza::ToolCall::Result { imza::ToolCall::Result::Kind::OUTPUT,
        "1\n" };
    done.result->dispatch_log
        = { { "read", "a.cpp", true }, { "shell", "ls", false } };
    CHECK(imza::tool_call_head(done) == "Lua · 2 tools (1 failed)");
    CHECK(imza::tool_header_args(done) == "2 tools (1 failed)");
    CHECK(imza::tool_header_args(done).find("print") == std::string::npos);

    CHECK(imza::lua_viewer_content(done).find(
              "Bindings: 1 read · 1 shell (1 failed)")
        != std::string::npos);
}

TEST_CASE("ask_answer_markdown numbers questions and blockquotes answers")
{
    imza::ModalAnswer ans;
    ans.cards.push_back(
        imza::QuestionAnswer { { "Sunny" }, "", "What's the weather today?" });
    ans.cards.push_back(
        imza::QuestionAnswer { { "Reading files", "Listing directories" }, "",
            "Which capabilities?" });

    const std::string md = imza::ask_answer_markdown(ans);
    CHECK(md
        == "1. **What's the weather today?**\n> Sunny\n"
           "2. **Which capabilities?**\n> Reading files, Listing directories");

    imza::ModalAnswer empty;
    empty.cards.push_back(imza::QuestionAnswer { { }, "", "Anything else?" });
    CHECK(imza::ask_answer_markdown(empty) == "1. **Anything else?**\n> -");
}

TEST_CASE("shell status text hides success and preserves arbitrary timeout")
{
    CHECK(imza::shell_status_text(imza::ShellExit { 0 }).empty());
    CHECK(
        imza::shell_status_text(imza::ShellExit { 7 }) == "exited with code 7");
    CHECK(imza::shell_status_text(
              imza::ShellTimeout { std::chrono::seconds { 47 } })
        == "timed out after 47s");
}
