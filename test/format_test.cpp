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
    CHECK(md.find("—") != std::string::npos);
}

TEST_CASE("tool_display_name capitalizes the first letter")
{
    CHECK(imza::tool_display_name("read") == "Read");
    CHECK(imza::tool_display_name("list") == "List");
    CHECK(imza::tool_display_name("") == "");
    CHECK(imza::tool_display_name("read_file") == "Read_file");
}

TEST_CASE("tool_args_summary flattens object args to key=value pairs")
{
    CHECK(imza::tool_args_summary(R"({"path":"notes.txt","n":3})")
        == "n=3 path=notes.txt");
    CHECK(imza::tool_args_summary(R"({"flag":true})") == "flag=true");
    CHECK(imza::tool_args_summary(R"({"path":null})") == "path=null");
}

TEST_CASE("tool_args_summary passes non-object args through verbatim")
{
    CHECK(imza::tool_args_summary("ls -la") == "ls -la");
    CHECK(imza::tool_args_summary("{}") == "{}");
}

TEST_CASE("tool_call_head shows the file path for read, args otherwise")
{
    imza::ToolCall read { 1, "", "read",
        R"({"path":"src/a.cpp","line_begin":1})", { } };
    CHECK(imza::tool_call_head(read) == "src/a.cpp");

    imza::ToolCall other { 1, "", "bash", "git status", { } };
    CHECK(imza::tool_call_head(other) == "Bash git status");

    imza::ToolCall ask { 1, "", "ask",
        R"({"questions":[{"prompt":"Continue?"}]})", { } };
    CHECK(imza::tool_call_head(ask) == "Ask (1 question)");

    imza::ToolCall ask_multi { 1, "", "ask",
        R"({"questions":[{"prompt":"A"},{"prompt":"B"}]})", { } };
    CHECK(imza::tool_call_head(ask_multi) == "Ask (2 questions)");

    imza::ToolCall todo { 1, "", "todo",
        R"({"todos":[{"content":"a","status":"pending"},{"content":"b","status":"in_progress"},{"content":"c","status":"completed"}]})",
        { } };
    CHECK(imza::tool_call_head(todo) == "Todo (3 tasks)");

    imza::ToolCall todo_one { 1, "", "todo", R"({"todos":[{"content":"a"}]})",
        { } };
    CHECK(imza::tool_call_head(todo_one) == "Todo (1 task)");

    imza::ToolCall todo_clear { 1, "", "todo", R"({"todos":[]})", { } };
    CHECK(imza::tool_call_head(todo_clear) == "Todo");

    imza::ToolCall todo_bad { 1, "", "todo", "not json", { } };
    CHECK(imza::tool_call_head(todo_bad) == "Todo");

    imza::ToolCall skill { 1, "", "skill",
        R"({"name":"code-review","scope":"project"})", { } };
    CHECK(imza::tool_call_head(skill) == "Load Skill code-review");
    CHECK(imza::tool_header_args(skill) == "code-review");

    imza::ToolCall subagent { 1, "", "subagent",
        R"({"tasks":[{"mode":"research","prompt":"Inspect parser behavior"},{"mode":"build","prompt":"Implement the fix"}]})",
        { } };
    CHECK(imza::tool_call_head(subagent) == "Subagent");
    CHECK(imza::tool_header_args(subagent) == "1 research, 1 builder");
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
    CHECK(imza::ask_answer_markdown(empty) == "1. **Anything else?**\n> —");
}

TEST_CASE("tool_code_language derives the extension for read only")
{
    imza::ToolCall read { 1, "", "read", R"({"path":"src/a.cpp"})", { } };
    CHECK(imza::tool_code_language(read) == "cpp");

    imza::ToolCall no_ext { 1, "", "read", R"({"path":"Makefile"})", { } };
    CHECK(imza::tool_code_language(no_ext).empty());

    imza::ToolCall other { 1, "", "bash", "git status", { } };
    CHECK(imza::tool_code_language(other).empty());
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
