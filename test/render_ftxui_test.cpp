#include <string>

#include <doctest/doctest.h>
#include <ftxui/component/component.hpp>

#include "test_helpers.h"
#include "ui/ui.h"
#include "workspace/git.h"

using imza::test::to_text;

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
    CHECK(imza::fit("abcdefgh", 4, 3) == "def…");
    CHECK(imza::fit("●alpha", 4, 1) == "alp…");
    CHECK(imza::fit("abcdefgh", 4, 6) == "gh  ");
}

TEST_CASE("render_markdown_element renders paragraphs")
{
    const std::string out
        = to_text(imza::render_markdown_element("hello world"));
    CHECK(out.find("hello") != std::string::npos);
}

TEST_CASE("render_markdown_element renders code blocks")
{
    const std::string out
        = to_text(imza::render_markdown_element("```\nint x = 42;\n```"));
    CHECK(out.find("int x = 42;") != std::string::npos);
    CHECK(out.find("┌") != std::string::npos);
}

TEST_CASE("C++ syntax highlighting uses standard terminal colors")
{
    auto screen = imza::test::to_screen(
        imza::highlight_code_line("return 42; // done", "cpp"), 24, 1);
    CHECK(screen.PixelAt(0, 0).foreground_color == ftxui::Color::Green);
    CHECK(screen.PixelAt(7, 0).foreground_color == ftxui::Color::Magenta);
    CHECK(screen.PixelAt(11, 0).foreground_color == ftxui::Color::GrayLight);
}

TEST_CASE("typed markdown fences use syntax highlighting")
{
    auto screen = imza::test::to_screen(
        imza::render_markdown_element("```cpp\nreturn 42;\n```"), 24, 4);
    CHECK(screen.PixelAt(1, 1).foreground_color == ftxui::Color::Green);
    CHECK(screen.PixelAt(8, 1).foreground_color == ftxui::Color::Magenta);
}

TEST_CASE("syntax type detection recognizes canonical names and extensions")
{
    CHECK(imza::syntax_type_supported("cpp"));
    CHECK(imza::syntax_type_supported("hpp"));
    CHECK(imza::syntax_type_for_path("src/example.cpp") == "cpp");
    CHECK(imza::syntax_type_for_path("include/example.h") == "cpp");
    CHECK_FALSE(imza::syntax_type_supported("c++"));
    CHECK_FALSE(imza::syntax_type_supported("txt"));
    CHECK(imza::syntax_type_for_path("notes.txt").empty());
}

TEST_CASE("syntax registry recognizes canonical languages and special files")
{
    CHECK(imza::syntax_type_supported("javascript"));
    CHECK(imza::syntax_type_supported("js"));
    CHECK(imza::syntax_type_for_path("Dockerfile") == "dockerfile");
    CHECK(imza::syntax_type_for_path("CMakeLists.txt") == "cmake");
    CHECK(imza::syntax_type_for_path("Makefile") == "make");
    CHECK(imza::syntax_type_for_path(".bashrc") == "bash");
    CHECK_FALSE(imza::syntax_type_supported("golang"));
    CHECK_FALSE(imza::syntax_type_supported("makefile"));
    CHECK_FALSE(imza::syntax_type_supported("sql"));
}

TEST_CASE("Tree-sitter highlight predicates filter C++ constants")
{
    auto screen = imza::test::to_screen(
        imza::highlight_code_line("value + MAX_VALUE", "cpp"), 24, 1);
    CHECK(screen.PixelAt(0, 0).foreground_color == imza::PANEL_FG);
    CHECK(screen.PixelAt(8, 0).foreground_color == ftxui::Color::Magenta);
}

TEST_CASE("Tree-sitter highlights multiline syntax in one parse")
{
    const auto lines = imza::highlight_code("/* first\nsecond */", "cpp");
    REQUIRE(lines.size() == 2);
    auto first  = imza::test::to_screen(lines[0], 16, 1);
    auto second = imza::test::to_screen(lines[1], 16, 1);
    CHECK(first.PixelAt(0, 0).foreground_color == ftxui::Color::GrayLight);
    CHECK(second.PixelAt(0, 0).foreground_color == ftxui::Color::GrayLight);
}

TEST_CASE("review highlighting parses old and new hunk sides as documents")
{
    imza::RepositoryReview review;
    imza::ReviewFile file;
    file.new_path = "example.cpp";
    imza::ReviewHunk hunk;
    hunk.lines = {
        { imza::ReviewLine::Kind::CONTEXT, 1, 1, "/* first" },
        { imza::ReviewLine::Kind::DELETION, 2, std::nullopt, "old */" },
        { imza::ReviewLine::Kind::ADDITION, std::nullopt, 2, "new */" },
    };
    file.hunks.push_back(std::move(hunk));
    review.files.push_back(std::move(file));

    imza::ReviewHighlights highlights;
    const auto& lines = review.files[0].hunks[0].lines;
    imza::append_review_hunk_highlights(
        highlights, review.files[0].hunks[0], "example.cpp", 80, 0, false);
    REQUIRE(highlights.contains(&lines[0]));
    REQUIRE(highlights.contains(&lines[1]));
    REQUIRE(highlights.contains(&lines[2]));
    const auto old_line
        = imza::test::to_screen(highlights.at(&lines[1]).old_side, 16, 1);
    const auto new_line
        = imza::test::to_screen(highlights.at(&lines[2]).new_side, 16, 1);
    CHECK(old_line.PixelAt(0, 0).foreground_color == ftxui::Color::GrayLight);
    CHECK(new_line.PixelAt(0, 0).foreground_color == ftxui::Color::GrayLight);
}

TEST_CASE("selected review changes use the cursor background")
{
    auto selected = imza::test::to_screen(
        imza::review_line_background(
            imza::highlight_code_line("return 1;", "cpp"),
            imza::DIFF_ADDITION_BG, true),
        16, 1);

    CHECK(selected.PixelAt(0, 0).foreground_color == ftxui::Color::Green);
    CHECK(selected.PixelAt(0, 0).background_color == imza::PANEL_COLOR_FOCUS);
}

TEST_CASE("unselected review changes retain their diff backgrounds")
{
    auto addition = imza::test::to_screen(
        imza::review_line_background(
            imza::highlight_code_line("return 1;", "cpp"),
            imza::DIFF_ADDITION_BG, false),
        16, 1);
    auto deletion = imza::test::to_screen(
        imza::review_line_background(
            imza::highlight_code_line("return 0;", "cpp"),
            imza::DIFF_DELETION_BG, false),
        16, 1);

    CHECK(addition.PixelAt(0, 0).background_color == imza::DIFF_ADDITION_BG);
    CHECK(deletion.PixelAt(0, 0).background_color == imza::DIFF_DELETION_BG);
}

TEST_CASE("untyped code keeps the panel foreground")
{
    auto screen = imza::test::to_screen(
        imza::highlight_code_line("return 42;", ""), 16, 1);
    CHECK(screen.PixelAt(0, 0).foreground_color == imza::PANEL_FG);
    CHECK(screen.PixelAt(7, 0).foreground_color == imza::PANEL_FG);
}

TEST_CASE("render_markdown_element spaces inline code from neighbors")
{
    const std::string out
        = to_text(imza::render_markdown_element("see `code` now"));
    CHECK(out.find("see ") != std::string::npos);
    CHECK(out.find(" now") != std::string::npos);
    CHECK(out.find("seecode") == std::string::npos);
    CHECK(out.find("codenow") == std::string::npos);
}

TEST_CASE("render_markdown_element renders tables")
{
    const std::string out
        = to_text(imza::render_markdown_element("| a | b |\n"
                                                "| - | - |\n"
                                                "| 1 | 2 |\n"));
    CHECK(out.find("a") != std::string::npos);
    CHECK(out.find("│") != std::string::npos);
}

TEST_CASE("render_markdown_element renders lists and headings")
{
    const std::string out = to_text(
        imza::render_markdown_element("# Title\n\n- one\n- two\n\n1. first\n"));
    CHECK(out.find("Title") != std::string::npos);
    CHECK(out.find("- one") != std::string::npos);
    CHECK(out.find("1. first") != std::string::npos);
}

TEST_CASE("render_markdown_element drops html")
{
    const std::string out
        = to_text(imza::render_markdown_element("text <script>bad</script>"));
    CHECK(out.find("<script>") == std::string::npos);
    CHECK(out.find("text") != std::string::npos);
}

TEST_CASE("render_markdown_element empty input")
{
    const std::string out = to_text(imza::render_markdown_element(""));
    CHECK(!out.empty());
}

TEST_CASE("session error element renders a full-width error bar")
{
    imza::Session session;
    session.set_error("add a review comment before sending");
    auto screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(60), ftxui::Dimension::Fixed(1));
    ftxui::Render(screen, imza::session_error_element(session));

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
        &content, imza::multiline_field_option(&content, &cursor, "Comment"));
    auto screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(20), ftxui::Dimension::Fixed(2));
    ftxui::Render(
        screen, input->Render() | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 20));
    CHECK_FALSE(screen.PixelAt(0, 0).underlined);
    CHECK(screen.PixelAt(0, 0).foreground_color == imza::PANEL_FG);
    CHECK(screen.PixelAt(0, 1).foreground_color == imza::PANEL_FG);
}

TEST_CASE("diff_split renders review-style side-by-side changes")
{
    imza::DiffView diff { "file.cpp",
        {
            { imza::DiffRow::Kind::REMOVE, 9, std::nullopt, "old", "" },
            { imza::DiffRow::Kind::ADD, 10, 10, "before", "after" },
        } };
    const std::string out = without_ansi(to_text(imza::diff_split(diff)));
    CHECK(out.find(" 9 − old") != std::string::npos);
    CHECK(out.find("10 − before") != std::string::npos);
    CHECK(out.find("10 + after") != std::string::npos);
}

TEST_CASE("diff_split renders unified changes on narrow screens")
{
    imza::DiffView diff { "file.cpp",
        {
            { imza::DiffRow::Kind::ADD, 10, 10, "before", "after" },
        } };
    const std::string out = without_ansi(to_text(imza::diff_split(diff, 80)));
    CHECK(out.find("10    − before") != std::string::npos);
    CHECK(out.find("   10 + after") != std::string::npos);
}

TEST_CASE("diff_split combines syntax foregrounds with change backgrounds")
{
    imza::DiffView diff { "file.cpp",
        {
            { imza::DiffRow::Kind::ADD, 1, 1, "return 0;", "return 1;" },
        } };
    auto screen = imza::test::to_screen(imza::diff_split(diff, 40), 40, 2);
    CHECK(screen.PixelAt(6, 0).foreground_color == ftxui::Color::Green);
    CHECK(screen.PixelAt(6, 0).background_color == imza::DIFF_DELETION_BG);
    CHECK(screen.PixelAt(6, 1).foreground_color == ftxui::Color::Green);
    CHECK(screen.PixelAt(6, 1).background_color == imza::DIFF_ADDITION_BG);
}
