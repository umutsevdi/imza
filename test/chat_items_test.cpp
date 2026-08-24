#include <string>

#include <doctest/doctest.h>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include "format.h"
#include "ui.h"

namespace {

std::string to_text(ftxui::Element element)
{
    using namespace ftxui;
    auto screen = Screen::Create(Dimension::Fixed(60), Dimension::Fixed(30));
    Render(screen, element);
    return screen.ToString();
}

std::string plain(std::string text)
{
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
            const size_t end = text.find('m', i);
            if (end != std::string::npos) {
                i = end;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

} // namespace

TEST_CASE("render_item renders a user turn")
{
    ursa::ConversationItem it = ursa::UserTurn { "hello" };
    const std::string out
        = to_text(ursa::render_item(it, { ursa::LayoutCtx::Kind::WIDE, 60 }));
    CHECK(out.find("hello") != std::string::npos);
}

TEST_CASE("render_item renders assistant markdown")
{
    ursa::ConversationItem it = ursa::AssistantTurn { "# Title\n" };
    const std::string out
        = to_text(ursa::render_item(it, { ursa::LayoutCtx::Kind::WIDE, 60 }));
    CHECK(out.find("Title") != std::string::npos);
}

TEST_CASE("render_item renders a modal answer and a tool call")
{
    ursa::ConversationItem ans = ursa::ModalAnswer { { { { "opt" }, "" } } };
    const std::string out_a
        = to_text(ursa::render_item(ans, { ursa::LayoutCtx::Kind::WIDE, 60 }));
    CHECK(out_a.find("User answered:") != std::string::npos);
    CHECK(out_a.find("opt") != std::string::npos);

    ursa::ConversationItem call
        = ursa::ToolCall { 1, "", "bash", "ls", std::nullopt };
    const std::string out_t
        = to_text(ursa::render_item(call, { ursa::LayoutCtx::Kind::WIDE, 60 }));
    CHECK(out_t.find("ls") != std::string::npos);
}

TEST_CASE("render_item renders tool call header and output code block")
{
    ursa::ToolCall call { 1, "", "read",
        R"({"path":"notes.txt","line_begin":2,"line_end":4})", { } };
    const std::string pending = to_text(
        ursa::render_item(call, { ursa::LayoutCtx::Kind::WIDE, 60 }));
    CHECK(pending.find("Tool Call:") != std::string::npos);
    CHECK(pending.find("Read notes.txt") != std::string::npos);
    CHECK(pending.find("line_begin=") == std::string::npos);

    call.result = ursa::ToolCall::Result { ursa::ToolCall::Result::Kind::OUTPUT,
        "line two\n  indented line three" };
    const std::string done = to_text(
        ursa::render_item(call, { ursa::LayoutCtx::Kind::WIDE, 60 }));
    CHECK(done.find("line two") != std::string::npos);
    CHECK(done.find("  indented line three") != std::string::npos);
    CHECK(plain(done).find("\n   txt") != std::string::npos);
}

TEST_CASE("render_item renders tool errors and rejections")
{
    ursa::ToolCall call { 1, "", "bash", "git status", { } };
    const std::string head = to_text(
        ursa::render_item(call, { ursa::LayoutCtx::Kind::WIDE, 60 }));
    CHECK(head.find("Bash git status") != std::string::npos);

    call.result = ursa::ToolCall::Result { ursa::ToolCall::Result::Kind::ERROR,
        "bash: command failed" };
    const std::string errored = to_text(
        ursa::render_item(call, { ursa::LayoutCtx::Kind::WIDE, 60 }));
    CHECK(errored.find("Error:") != std::string::npos);
    CHECK(errored.find("bash: command failed") != std::string::npos);

    call.result = ursa::ToolCall::Result { ursa::ToolCall::Result::Kind::REJECT,
        "needs approval first" };
    const std::string rejected = to_text(
        ursa::render_item(call, { ursa::LayoutCtx::Kind::WIDE, 60 }));
    CHECK(rejected.find("Rejected:") != std::string::npos);
    CHECK(rejected.find("needs approval first") != std::string::npos);
}
