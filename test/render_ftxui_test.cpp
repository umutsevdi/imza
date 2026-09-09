#include <string>
#include <utility>
#include <vector>

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

TEST_CASE("fit truncates with an ellipsis and pads to width")
{
    CHECK(imza::fit("abcdefgh", 4) == "abc…");
    CHECK(imza::fit("●alpha", 2) == "●…");
    CHECK(imza::fit("abc", 5) == "abc  ");
}

TEST_CASE("wrap_row_ranges wraps to display width")
{
    using R         = std::pair<std::size_t, std::size_t>;
    const auto wide = std::string_view("漢漢漢");
    CHECK(imza::wrap_row_ranges("aaaa bbbb cccc", 9)
        == std::vector<R> { { 0, 5 }, { 5, 14 } });
    CHECK(imza::wrap_row_ranges("abcdefgh", 4)
        == std::vector<R> { { 0, 4 }, { 4, 8 } });
    CHECK(imza::wrap_row_ranges("exact", 5) == std::vector<R> { { 0, 5 } });
    CHECK(imza::wrap_row_ranges("", 4) == std::vector<R> { { 0, 0 } });
    CHECK(imza::wrap_row_ranges(wide, 4)
        == std::vector<R> { { 0, 6 }, { 6, 9 } });
}

TEST_CASE("wrap_text wraps every logical line")
{
    CHECK(imza::wrap_text("one two three four\n\nlast", 8)
        == std::vector<std::string> {
            "one two ", "three ", "four", "", "last" });
    CHECK(imza::wrap_text("", 8) == std::vector<std::string> { "" });
}

TEST_CASE("render_markdown_element renders paragraphs")
{
    const std::string out
        = to_text(imza::render_markdown_element("hello world", 60));
    CHECK(out.find("hello") != std::string::npos);
}

TEST_CASE("render_markdown_element renders code blocks")
{
    const std::string out
        = to_text(imza::render_markdown_element("```\nint x = 42;\n```", 60));
    CHECK(out.find("int x = 42;") != std::string::npos);
    CHECK(out.find("┌") != std::string::npos);
}

TEST_CASE("markdown code blocks wrap long lines")
{
    const std::string md = "```\n" + std::string(80, 'x') + " tail\n```";
    const std::string out
        = to_text(imza::render_markdown_element(md, 60), 60, 8);
    CHECK(out.find("tail") != std::string::npos);
}

TEST_CASE("highlight_code_wrapped keeps token colors across wrapped rows")
{
    const std::string code = "return \"aaaaaaaaaaaaaaaa\";";
    auto whole             = imza::test::to_screen(
        imza::highlight_code_line(code, "cpp"), code.size(), 1);
    const ftxui::Color keyword = whole.PixelAt(0, 0).foreground_color;
    const ftxui::Color literal = whole.PixelAt(10, 0).foreground_color;

    const auto rows = imza::highlight_code_wrapped(code, "cpp", 10);
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].size() == 3);
    auto first  = imza::test::to_screen(rows[0][0], 10, 1);
    auto second = imza::test::to_screen(rows[0][1], 10, 1);
    auto third  = imza::test::to_screen(rows[0][2], 10, 1);
    CHECK(first.PixelAt(0, 0).foreground_color == keyword);
    CHECK(second.PixelAt(1, 0).foreground_color == literal);
    CHECK(third.PixelAt(0, 0).foreground_color == literal);
}

TEST_CASE("review highlighting wraps long hunk lines")
{
    imza::ReviewLine line;
    line.kind     = imza::ReviewLine::Kind::ADDITION;
    line.content  = "auto message = \"" + std::string(60, 'a') + "\";";
    line.new_line = 3;
    imza::ReviewHunk hunk;
    hunk.lines.push_back(line);
    imza::ReviewHighlights cache;
    imza::append_review_hunk_highlights(cache, hunk, "file.cpp", 60, false);
    REQUIRE(cache.size() == 1);
    const std::vector<ftxui::Element>& rows = cache.begin()->second.new_side;
    REQUIRE(rows.size() == 3);
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
        = to_text(imza::render_markdown_element("see `code` now", 60));
    CHECK(out.find("see ") != std::string::npos);
    CHECK(out.find(" now") != std::string::npos);
    CHECK(out.find("seecode") == std::string::npos);
    CHECK(out.find("codenow") == std::string::npos);
}

TEST_CASE("render_markdown_element renders tables")
{
    const std::string out = to_text(imza::render_markdown_element("| a | b |\n"
                                                                  "| - | - |\n"
                                                                  "| 1 | 2 |\n",
        60));
    CHECK(out.find("a") != std::string::npos);
    CHECK(out.find("│") != std::string::npos);
}

TEST_CASE("render_markdown_element renders lists and headings")
{
    const std::string out = to_text(imza::render_markdown_element(
        "# Title\n\n- one\n- two\n\n1. first\n", 60));
    CHECK(out.find("Title") != std::string::npos);
    CHECK(out.find("- one") != std::string::npos);
    CHECK(out.find("1. first") != std::string::npos);
}

TEST_CASE("render_markdown_element drops html")
{
    const std::string out = to_text(
        imza::render_markdown_element("text <script>bad</script>", 60));
    CHECK(out.find("<script>") == std::string::npos);
    CHECK(out.find("text") != std::string::npos);
}

TEST_CASE("render_markdown_element empty input")
{
    const std::string out = to_text(imza::render_markdown_element("", 60));
    CHECK(!out.empty());
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

TEST_CASE("diff_split wraps long lines instead of clipping them")
{
    const std::string long_line
        = "right side of the diff has quite a long unwrapped line here";
    imza::DiffView unified { "file.cpp",
        {
            { imza::DiffRow::Kind::ADD, 10, 10, "", long_line },
        } };
    const std::string narrow
        = without_ansi(to_text(imza::diff_split(unified, 40), 40, 6));
    CHECK(narrow.find("right side") != std::string::npos);
    CHECK(narrow.find("here") != std::string::npos);

    imza::DiffView side_by_side { "file.cpp",
        {
            { imza::DiffRow::Kind::ADD, 10, 10, "", long_line },
        } };
    const std::string wide
        = without_ansi(to_text(imza::diff_split(side_by_side), 120, 6));
    CHECK(wide.find("right side") != std::string::npos);
    CHECK(wide.find("here") != std::string::npos);
}
