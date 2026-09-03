#include <string>

#include <doctest/doctest.h>

#include "subsystems/format.h"
#include "test_helpers.h"
#include "ui/ui.h"

using ursa::test::to_text;

TEST_CASE("render_item renders a user turn")
{
    ursa::ConversationItem it = ursa::UserTurn { "hello" };
    const std::string out
        = to_text(ursa::render_item(it, { ursa::LayoutCtx::Kind::WIDE, 60 }));
    CHECK(out.find("hello") != std::string::npos);
}

TEST_CASE("render_item renders user attachment labels")
{
    ursa::ConversationItem it
        = ursa::UserTurn { "review", { { "src/main.cpp", "int main() {}" } } };
    const std::string out
        = to_text(ursa::render_item(it, { ursa::LayoutCtx::Kind::WIDE, 60 }));
    CHECK(out.find("@src/main.cpp") != std::string::npos);
}

TEST_CASE("render_item renders assistant markdown")
{
    ursa::ConversationItem it = ursa::AssistantTurn { "# Title\n" };
    const std::string out
        = to_text(ursa::render_item(it, { ursa::LayoutCtx::Kind::WIDE, 60 }));
    CHECK(out.find("Title") != std::string::npos);
}

TEST_CASE("render_item renders a modal answer")
{
    ursa::ConversationItem ans = ursa::ModalAnswer { { { { "opt" }, "" } } };
    const std::string out_a
        = to_text(ursa::render_item(ans, { ursa::LayoutCtx::Kind::WIDE, 60 }));
    CHECK(out_a.find("User answered:") != std::string::npos);
    CHECK(out_a.find("opt") != std::string::npos);
}

TEST_CASE("render_item renders completed compaction")
{
    ursa::ConversationItem item
        = ursa::CompactionEvent { 1, ursa::CompactionEvent::Status::COMPLETED };
    const std::string out
        = to_text(ursa::render_item(item, { ursa::LayoutCtx::Kind::WIDE, 60 }));
    CHECK(out.find("✓ Session compacted") != std::string::npos);
}
