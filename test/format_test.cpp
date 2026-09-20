#include <string>

#include <doctest/doctest.h>

#include "conversation/format.h"
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
