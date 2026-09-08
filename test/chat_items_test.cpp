#include <string>

#include <doctest/doctest.h>

#include "conversation/format.h"
#include "test_helpers.h"
#include "ui/ui.h"

using imza::test::to_text;

TEST_CASE("render_item renders a user turn")
{
    imza::ConversationItem it = imza::UserTurn { "hello" };
    const std::string out
        = to_text(imza::render_item(it, { imza::LayoutCtx::Kind::WIDE, 60 }));
    CHECK(out.find("hello") != std::string::npos);
}

TEST_CASE("render_item renders user attachment labels")
{
    imza::ConversationItem it
        = imza::UserTurn { "review", { { "src/main.cpp", "int main() {}" } } };
    const std::string out
        = to_text(imza::render_item(it, { imza::LayoutCtx::Kind::WIDE, 60 }));
    CHECK(out.find("@src/main.cpp") != std::string::npos);
}

TEST_CASE("render_item renders assistant markdown")
{
    imza::ConversationItem it = imza::AssistantTurn { "# Title\n" };
    const std::string out
        = to_text(imza::render_item(it, { imza::LayoutCtx::Kind::WIDE, 60 }));
    CHECK(out.find("Title") != std::string::npos);
}

TEST_CASE("render_item renders a modal answer")
{
    imza::ConversationItem ans = imza::ModalAnswer { { { { "opt" }, "" } } };
    const std::string out_a
        = to_text(imza::render_item(ans, { imza::LayoutCtx::Kind::WIDE, 60 }));
    CHECK(out_a.find("User answered:") != std::string::npos);
    CHECK(out_a.find("opt") != std::string::npos);
}

TEST_CASE("render_item renders completed compaction")
{
    imza::ConversationItem item
        = imza::CompactionEvent { 1, imza::CompactionEvent::Status::COMPLETED };
    const std::string out
        = to_text(imza::render_item(item, { imza::LayoutCtx::Kind::WIDE, 60 }));
    CHECK(out.find("✓ Session compacted") != std::string::npos);
}

TEST_CASE("virtual list selects only the visible rows")
{
    imza::VirtualListState list(3);
    list.resize(100);

    const imza::VirtualListWindow window = list.window(150, 12, 0);

    CHECK(window.begin == 50);
    CHECK(window.end == 54);
    CHECK(window.before == 150);
    CHECK(window.after == 138);
    CHECK(list.total_height() == 300);
}

TEST_CASE("virtual list preserves measured heights and applies overscan")
{
    imza::VirtualListState list(3);
    list.resize(5);
    CHECK(list.set_height(0, 8) == 5);
    CHECK(list.set_height(1, 5) == 2);
    CHECK(list.set_height(1, 5) == 0);

    const imza::VirtualListWindow window = list.window(9, 3, 4);

    CHECK(window.begin == 0);
    CHECK(window.end == 3);
    CHECK(window.before == 0);
    CHECK(window.after == 6);
    CHECK(list.total_height() == 22);
}

TEST_CASE("virtual list retains measurements while growing")
{
    imza::VirtualListState list(2);
    list.resize(2);
    list.set_height(0, 7);
    list.resize(4);

    CHECK(list.total_height() == 13);
    CHECK(list.window(7, 2, 0).begin == 1);

    list.resize(1);
    CHECK(list.total_height() == 7);
}
