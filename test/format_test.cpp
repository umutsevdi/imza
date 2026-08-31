#include <string>

#include <doctest/doctest.h>

#include "agent/format.h"
#include "ui/tool_format.h"

TEST_CASE("question_form_markdown renders prompt and options")
{
    ursa::QuestionForm form { { "model?", { "gpt-4o", "claude" }, false,
        false } };
    const std::string md = ursa::question_form_markdown(form);
    CHECK(md.find("Question: \"model?\"") != std::string::npos);
    CHECK(md.find("- gpt-4o") != std::string::npos);
    CHECK(md.find("- claude") != std::string::npos);
}

TEST_CASE("modal_answer_markdown renders selected then free text")
{
    ursa::ModalAnswer ans { { { { "Option 3" }, "extra note" } } };
    const std::string md = ursa::modal_answer_markdown(ans);
    CHECK(md.find("User answered:") == 0);
    CHECK(md.find("> Option 3\n> extra note") != std::string::npos);
}

TEST_CASE("modal_answer_markdown renders Q/A pairs with prompt")
{
    ursa::ModalAnswer ans;
    ans.cards.push_back(
        ursa::QuestionAnswer { { "PostgreSQL" }, "", "storage backend?" });
    ans.cards.push_back(
        ursa::QuestionAnswer { { "Auth", "Billing" }, "", "features?" });
    ans.cards.push_back(
        ursa::QuestionAnswer { { }, "my own region", "region?" });
    ans.cards.push_back(ursa::QuestionAnswer { { }, "", "anything else?" });
    const std::string md = ursa::modal_answer_markdown(ans);
    CHECK(md.find("**storage backend?**") != std::string::npos);
    CHECK(md.find("PostgreSQL") != std::string::npos);
    CHECK(md.find("Auth, Billing") != std::string::npos);
    CHECK(md.find("my own region") != std::string::npos);
    CHECK(md.find("anything else?**") != std::string::npos);
    CHECK(md.find("—") != std::string::npos);
}

TEST_CASE("tool_display_name capitalizes the first letter")
{
    CHECK(ursa::tool_display_name("read") == "Read");
    CHECK(ursa::tool_display_name("list") == "List");
    CHECK(ursa::tool_display_name("") == "");
    CHECK(ursa::tool_display_name("read_file") == "Read_file");
}

TEST_CASE("tool_args_summary flattens object args to key=value pairs")
{
    CHECK(ursa::tool_args_summary(R"({"path":"notes.txt","n":3})")
        == "n=3 path=notes.txt");
    CHECK(ursa::tool_args_summary(R"({"flag":true})") == "flag=true");
    CHECK(ursa::tool_args_summary(R"({"path":null})") == "path=null");
}

TEST_CASE("tool_args_summary passes non-object args through verbatim")
{
    CHECK(ursa::tool_args_summary("ls -la") == "ls -la");
    CHECK(ursa::tool_args_summary("{}") == "{}");
}

TEST_CASE("tool_call_head shows the file path for read, args otherwise")
{
    ursa::ToolCall read { 1, "", "read",
        R"({"path":"src/a.cpp","line_begin":1})", { } };
    CHECK(ursa::tool_call_head(read) == "src/a.cpp");

    ursa::ToolCall other { 1, "", "bash", "git status", { } };
    CHECK(ursa::tool_call_head(other) == "Bash git status");

    ursa::ToolCall ask { 1, "", "ask",
        R"({"questions":[{"prompt":"Continue?"}]})", { } };
    CHECK(ursa::tool_call_head(ask) == "Ask (1 question)");

    ursa::ToolCall ask_multi { 1, "", "ask",
        R"({"questions":[{"prompt":"A"},{"prompt":"B"}]})", { } };
    CHECK(ursa::tool_call_head(ask_multi) == "Ask (2 questions)");

    ursa::ToolCall todo { 1, "", "todo",
        R"({"todos":[{"content":"a","status":"pending"},{"content":"b","status":"in_progress"},{"content":"c","status":"completed"}]})",
        { } };
    CHECK(ursa::tool_call_head(todo) == "Todo (3 tasks)");

    ursa::ToolCall todo_one { 1, "", "todo", R"({"todos":[{"content":"a"}]})",
        { } };
    CHECK(ursa::tool_call_head(todo_one) == "Todo (1 task)");

    ursa::ToolCall todo_clear { 1, "", "todo", R"({"todos":[]})", { } };
    CHECK(ursa::tool_call_head(todo_clear) == "Todo");

    ursa::ToolCall todo_bad { 1, "", "todo", "not json", { } };
    CHECK(ursa::tool_call_head(todo_bad) == "Todo");

    ursa::ToolCall skill { 1, "", "skill",
        R"({"name":"code-review","scope":"project"})", { } };
    CHECK(ursa::tool_call_head(skill) == "Load Skill code-review");
    CHECK(ursa::tool_header_args(skill) == "code-review");

    ursa::ToolCall subagent { 1, "", "subagent",
        R"({"tasks":[{"mode":"research","prompt":"Inspect parser behavior"},{"mode":"build","prompt":"Implement the fix"}]})",
        { } };
    CHECK(ursa::tool_call_head(subagent) == "Subagent");
    CHECK(ursa::tool_header_args(subagent) == "1 research, 1 builder");
}

TEST_CASE("ask_answer_markdown numbers questions and blockquotes answers")
{
    ursa::ModalAnswer ans;
    ans.cards.push_back(
        ursa::QuestionAnswer { { "Sunny" }, "", "What's the weather today?" });
    ans.cards.push_back(
        ursa::QuestionAnswer { { "Reading files", "Listing directories" }, "",
            "Which capabilities?" });

    const std::string md = ursa::ask_answer_markdown(ans);
    CHECK(md
        == "1. **What's the weather today?**\n> Sunny\n"
           "2. **Which capabilities?**\n> Reading files, Listing directories");

    ursa::ModalAnswer empty;
    empty.cards.push_back(ursa::QuestionAnswer { { }, "", "Anything else?" });
    CHECK(ursa::ask_answer_markdown(empty) == "1. **Anything else?**\n> —");
}

TEST_CASE("tool_code_language derives the extension for read only")
{
    ursa::ToolCall read { 1, "", "read", R"({"path":"src/a.cpp"})", { } };
    CHECK(ursa::tool_code_language(read) == "cpp");

    ursa::ToolCall no_ext { 1, "", "read", R"({"path":"Makefile"})", { } };
    CHECK(ursa::tool_code_language(no_ext).empty());

    ursa::ToolCall other { 1, "", "bash", "git status", { } };
    CHECK(ursa::tool_code_language(other).empty());
}

TEST_CASE("shell status text hides success and preserves arbitrary timeout")
{
    CHECK(ursa::shell_status_text(ursa::ShellExit { 0 }).empty());
    CHECK(
        ursa::shell_status_text(ursa::ShellExit { 7 }) == "exited with code 7");
    CHECK(ursa::shell_status_text(
              ursa::ShellTimeout { std::chrono::seconds { 47 } })
        == "timed out after 47s");
}
