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
        = ursa::ToolCall { 1, "bash", "ls", std::nullopt };
    const std::string out_t
        = to_text(ursa::render_item(call, { ursa::LayoutCtx::Kind::WIDE, 60 }));
    CHECK(out_t.find("ls") != std::string::npos);
}

TEST_CASE("tool_call_markdown renders request, separator, output")
{
    ursa::ToolCall call { 1, "bash", "ls -la", { } };
    const std::string pending = ursa::tool_call_markdown(call);
    CHECK(pending.find("Requested to call:") == 0);
    CHECK(pending.find("```bash\nls -la\n```") != std::string::npos);
    CHECK(pending.find("---") == std::string::npos);

    call.result = ursa::ToolCall::Result { ursa::ToolCall::Result::Kind::OUTPUT,
        "total 76" };
    const std::string done = ursa::tool_call_markdown(call);
    CHECK(done.find("---") != std::string::npos);
    CHECK(done.find("total 76") != std::string::npos);
}

TEST_CASE("tool_call_markdown renders rejection with and without reason")
{
    ursa::ToolCall call { 1, "bash", "ls", { } };
    call.result = ursa::ToolCall::Result { ursa::ToolCall::Result::Kind::REJECT,
        "needs approval first" };
    const std::string with_reason = ursa::tool_call_markdown(call);
    CHECK(with_reason.find("User answered:") != std::string::npos);
    CHECK(with_reason.find("> Rejected: needs approval first")
        != std::string::npos);

    call.result->text      = "";
    const std::string bare = ursa::tool_call_markdown(call);
    CHECK(bare.find("> Rejected:") != std::string::npos);
    CHECK(bare.find("> Rejected: ") == std::string::npos);
}
