#include <string>

#include <doctest/doctest.h>
#include <ftxui/component/component.hpp>

#include "core/git.h"
#include "test_helpers.h"
#include "ui/ui.h"

using ursa::test::to_text;

namespace {

std::string without_ansi(std::string_view input)
{
    std::string out;
    for (std::size_t i = 0; i < input.size();) {
        if (input[i] != '\x1b' || i + 1 >= input.size()
            || input[i + 1] != '[') {
            out += input[i++];
            continue;
        }
        i += 2;
        while (i < input.size() && (input[i] < '@' || input[i] > '~')) {
            ++i;
        }
        if (i < input.size()) {
            ++i;
        }
    }
    return out;
}

} // namespace

TEST_CASE("fit supports UTF-8-aware horizontal offsets")
{
    CHECK(ursa::fit("abcdefgh", 4, 3) == "def…");
    CHECK(ursa::fit("●alpha", 4, 1) == "alp…");
    CHECK(ursa::fit("abcdefgh", 4, 6) == "gh  ");
}

TEST_CASE("render_markdown_element renders paragraphs")
{
    const std::string out
        = to_text(ursa::render_markdown_element("hello world"));
    CHECK(out.find("hello") != std::string::npos);
}

TEST_CASE("render_markdown_element renders code blocks")
{
    const std::string out
        = to_text(ursa::render_markdown_element("```\nint x = 42;\n```"));
    CHECK(out.find("int x = 42;") != std::string::npos);
    CHECK(out.find("┌") != std::string::npos);
}

TEST_CASE("C++ syntax highlighting uses standard terminal colors")
{
    auto screen = ursa::test::to_screen(
        ursa::highlight_code_line("return 42; // done", "cpp"), 24, 1);
    CHECK(screen.PixelAt(0, 0).foreground_color == ftxui::Color::Green);
    CHECK(screen.PixelAt(7, 0).foreground_color == ftxui::Color::Magenta);
    CHECK(screen.PixelAt(11, 0).foreground_color == ftxui::Color::GrayLight);
}

TEST_CASE("typed markdown fences use syntax highlighting")
{
    auto screen = ursa::test::to_screen(
        ursa::render_markdown_element("```cpp\nreturn 42;\n```"), 24, 4);
    CHECK(screen.PixelAt(1, 1).foreground_color == ftxui::Color::Green);
    CHECK(screen.PixelAt(8, 1).foreground_color == ftxui::Color::Magenta);
}

TEST_CASE("syntax type detection recognizes canonical names and extensions")
{
    CHECK(ursa::syntax_type_supported("cpp"));
    CHECK(ursa::syntax_type_supported("hpp"));
    CHECK(ursa::syntax_type_for_path("src/example.cpp") == "cpp");
    CHECK(ursa::syntax_type_for_path("include/example.h") == "cpp");
    CHECK_FALSE(ursa::syntax_type_supported("c++"));
    CHECK_FALSE(ursa::syntax_type_supported("txt"));
    CHECK(ursa::syntax_type_for_path("notes.txt").empty());
}

TEST_CASE("syntax registry recognizes canonical languages and special files")
{
    CHECK(ursa::syntax_type_supported("javascript"));
    CHECK(ursa::syntax_type_supported("js"));
    CHECK(ursa::syntax_type_for_path("Dockerfile") == "dockerfile");
    CHECK(ursa::syntax_type_for_path("CMakeLists.txt") == "cmake");
    CHECK(ursa::syntax_type_for_path("Makefile") == "make");
    CHECK(ursa::syntax_type_for_path(".bashrc") == "bash");
    CHECK_FALSE(ursa::syntax_type_supported("golang"));
    CHECK_FALSE(ursa::syntax_type_supported("makefile"));
    CHECK_FALSE(ursa::syntax_type_supported("sql"));
}

TEST_CASE("Tree-sitter highlight predicates filter C++ constants")
{
    auto screen = ursa::test::to_screen(
        ursa::highlight_code_line("value + MAX_VALUE", "cpp"), 24, 1);
    CHECK(screen.PixelAt(0, 0).foreground_color == ursa::PANEL_FG);
    CHECK(screen.PixelAt(8, 0).foreground_color == ftxui::Color::Magenta);
}

TEST_CASE("Tree-sitter highlights multiline syntax in one parse")
{
    const auto lines = ursa::highlight_code("/* first\nsecond */", "cpp");
    REQUIRE(lines.size() == 2);
    auto first  = ursa::test::to_screen(lines[0], 16, 1);
    auto second = ursa::test::to_screen(lines[1], 16, 1);
    CHECK(first.PixelAt(0, 0).foreground_color == ftxui::Color::GrayLight);
    CHECK(second.PixelAt(0, 0).foreground_color == ftxui::Color::GrayLight);
}

TEST_CASE("review highlighting parses old and new hunk sides as documents")
{
    ursa::RepositoryReview review;
    ursa::ReviewFile file;
    file.new_path = "example.cpp";
    ursa::ReviewHunk hunk;
    hunk.lines = {
        { ursa::ReviewLine::Kind::CONTEXT, 1, 1, "/* first" },
        { ursa::ReviewLine::Kind::DELETION, 2, std::nullopt, "old */" },
        { ursa::ReviewLine::Kind::ADDITION, std::nullopt, 2, "new */" },
    };
    file.hunks.push_back(std::move(hunk));
    review.files.push_back(std::move(file));

    ursa::ReviewHighlights highlights;
    const auto& lines = review.files[0].hunks[0].lines;
    ursa::append_review_hunk_highlights(
        highlights, review.files[0].hunks[0], "example.cpp", 80, 0, false);
    REQUIRE(highlights.contains(&lines[0]));
    REQUIRE(highlights.contains(&lines[1]));
    REQUIRE(highlights.contains(&lines[2]));
    const auto old_line
        = ursa::test::to_screen(highlights.at(&lines[1]).old_side, 16, 1);
    const auto new_line
        = ursa::test::to_screen(highlights.at(&lines[2]).new_side, 16, 1);
    CHECK(old_line.PixelAt(0, 0).foreground_color == ftxui::Color::GrayLight);
    CHECK(new_line.PixelAt(0, 0).foreground_color == ftxui::Color::GrayLight);
}

TEST_CASE("untyped code keeps the panel foreground")
{
    auto screen = ursa::test::to_screen(
        ursa::highlight_code_line("return 42;", ""), 16, 1);
    CHECK(screen.PixelAt(0, 0).foreground_color == ursa::PANEL_FG);
    CHECK(screen.PixelAt(7, 0).foreground_color == ursa::PANEL_FG);
}

TEST_CASE("render_markdown_element spaces inline code from neighbors")
{
    const std::string out
        = to_text(ursa::render_markdown_element("see `code` now"));
    CHECK(out.find("see ") != std::string::npos);
    CHECK(out.find(" now") != std::string::npos);
    CHECK(out.find("seecode") == std::string::npos);
    CHECK(out.find("codenow") == std::string::npos);
}

TEST_CASE("render_markdown_element renders tables")
{
    const std::string out
        = to_text(ursa::render_markdown_element("| a | b |\n"
                                                "| - | - |\n"
                                                "| 1 | 2 |\n"));
    CHECK(out.find("a") != std::string::npos);
    CHECK(out.find("│") != std::string::npos);
}

TEST_CASE("render_markdown_element renders lists and headings")
{
    const std::string out = to_text(
        ursa::render_markdown_element("# Title\n\n- one\n- two\n\n1. first\n"));
    CHECK(out.find("Title") != std::string::npos);
    CHECK(out.find("- one") != std::string::npos);
    CHECK(out.find("1. first") != std::string::npos);
}

TEST_CASE("render_markdown_element drops html")
{
    const std::string out
        = to_text(ursa::render_markdown_element("text <script>bad</script>"));
    CHECK(out.find("<script>") == std::string::npos);
    CHECK(out.find("text") != std::string::npos);
}

TEST_CASE("render_markdown_element empty input")
{
    const std::string out = to_text(ursa::render_markdown_element(""));
    CHECK(!out.empty());
}

TEST_CASE("session error element renders a full-width error bar")
{
    ursa::Session session;
    session.set_error("add a review comment before sending");
    auto screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(60), ftxui::Dimension::Fixed(1));
    ftxui::Render(screen, ursa::session_error_element(session));

    CHECK(screen.ToString().find("Add a review comment before sending.")
        != std::string::npos);
    CHECK(screen.PixelAt(0, 0).background_color == ftxui::Color::Red);
    CHECK(screen.PixelAt(59, 0).background_color == ftxui::Color::Red);
}

TEST_CASE("multiline field underlines only its last row")
{
    std::string content = "first\nsecond";
    int cursor          = static_cast<int>(content.size());
    const auto input    = ftxui::Input(
        &content, ursa::multiline_field_option(&content, &cursor, "Comment"));
    auto screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(20), ftxui::Dimension::Fixed(2));
    ftxui::Render(
        screen, input->Render() | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 20));
    CHECK_FALSE(screen.PixelAt(0, 0).underlined);
    CHECK(screen.PixelAt(0, 0).foreground_color == ursa::PANEL_FG);
    CHECK(screen.PixelAt(0, 1).foreground_color == ursa::PANEL_FG);
}

TEST_CASE("diff_split renders review-style side-by-side changes")
{
    ursa::DiffView diff { "file.cpp",
        {
            { ursa::DiffRow::Kind::REMOVE, 9, std::nullopt, "old", "" },
            { ursa::DiffRow::Kind::ADD, 10, 10, "before", "after" },
        } };
    const std::string out = without_ansi(to_text(ursa::diff_split(diff)));
    CHECK(out.find(" 9 − old") != std::string::npos);
    CHECK(out.find("10 − before") != std::string::npos);
    CHECK(out.find("10 + after") != std::string::npos);
}

TEST_CASE("diff_split renders unified changes on narrow screens")
{
    ursa::DiffView diff { "file.cpp",
        {
            { ursa::DiffRow::Kind::ADD, 10, 10, "before", "after" },
        } };
    const std::string out = without_ansi(to_text(ursa::diff_split(diff, 80)));
    CHECK(out.find("10    − before") != std::string::npos);
    CHECK(out.find("   10 + after") != std::string::npos);
}

TEST_CASE("diff_split combines syntax foregrounds with change backgrounds")
{
    ursa::DiffView diff { "file.cpp",
        {
            { ursa::DiffRow::Kind::ADD, 1, 1, "return 0;", "return 1;" },
        } };
    auto screen = ursa::test::to_screen(ursa::diff_split(diff, 40), 40, 2);
    CHECK(screen.PixelAt(6, 0).foreground_color == ftxui::Color::Green);
    CHECK(screen.PixelAt(6, 0).background_color == ursa::DIFF_DELETION_BG);
    CHECK(screen.PixelAt(6, 1).foreground_color == ftxui::Color::Green);
    CHECK(screen.PixelAt(6, 1).background_color == ursa::DIFF_ADDITION_BG);
}
