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
    CHECK(imza::wrap_text("last\n", 8)
        == std::vector<std::string> { "last", "" });
    CHECK(imza::wrap_text("", 8) == std::vector<std::string> { "" });
}

TEST_CASE("wrapped input keeps long draft text visible")
{
    const std::string draft = "one two three four";
    const std::string out
        = without_ansi(to_text(imza::wrapped_input_element(draft, draft.size(),
                                   8, "Leave a comment", false),
            8, 4));

    CHECK(out.find("one two") != std::string::npos);
    CHECK(out.find("three") != std::string::npos);
    CHECK(out.find("four") != std::string::npos);
}

TEST_CASE("wrapped input preserves UTF-8 text around the cursor")
{
    const std::string draft = "prefix 漢漢 suffix";
    const std::string out   = without_ansi(to_text(
        imza::wrapped_input_element(draft, draft.find(" suffix"), 9, "", false),
        9, 3));

    CHECK(out.find("prefix") != std::string::npos);
    CHECK(out.find("漢漢") != std::string::npos);
    CHECK(out.find("suffix") != std::string::npos);
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
    const auto unwrapped   = imza::highlight_code_wrapped(
        code, "cpp", static_cast<int>(code.size()));
    REQUIRE(unwrapped.size() == 1);
    auto whole = imza::test::to_screen(unwrapped[0][0], code.size(), 1);
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
TEST_CASE("diff_split renders skip rows as elision markers")
{
    imza::DiffView diff { "file.cpp",
        {
            { imza::DiffRow::Kind::SAME, 1, 1, "keep", "keep" },
            { imza::DiffRow::Kind::SKIP, 5, 5, "… 8 unchanged line(s) …",
                "… 8 unchanged line(s) …" },
            { imza::DiffRow::Kind::ADD, { }, 14, "", "new" },
        } };
    const std::string out = without_ansi(to_text(imza::diff_split(diff)));
    CHECK(out.find("8 unchanged line(s)") != std::string::npos);
    CHECK(out.find("14 + new") != std::string::npos);
}
TEST_CASE("markdown alert quote drops the [!ERROR] marker and keeps content")
{
    const std::string out = to_text(
        imza::render_markdown_element("> [!ERROR]\n> disk full\n", 60), 60, 8);
    CHECK(out.find("[!ERROR]") == std::string::npos);
    CHECK(out.find("disk full") != std::string::npos);

    const std::string plain
        = to_text(imza::render_markdown_element("> note\n", 60), 60, 8);
    CHECK(plain.find("note") != std::string::npos);
}
TEST_CASE("capability tags join advertised modalities with separators")
{
    using imza::Capabilities;
    CHECK(imza::capability_tags(Capabilities::IMAGE | Capabilities::PDF)
        == "image · pdf");
    CHECK(imza::capability_tags(Capabilities::IMAGE) == "image");
    CHECK(imza::capability_tags(Capabilities::PDF) == "pdf");
    CHECK(imza::capability_tags(Capabilities::NONE).empty());
    CHECK(imza::capability_tags(std::nullopt).empty());
}

TEST_CASE("model picker rows display advertised capability tags")
{
    imza::ModelInfo info;
    info.id           = "openai/gpt-5.5";
    info.capabilities = imza::Capabilities::IMAGE | imza::Capabilities::PDF;
    const std::string tagged = without_ansi(to_text(imza::model_picker_row(
        imza::make_model_row("c", "OpenAI", info), false)));
    CHECK(tagged.find("gpt-5.5") != std::string::npos);
    CHECK(tagged.find("image · pdf · openai") != std::string::npos);

    imza::ModelInfo image_only;
    image_only.id           = "m1";
    image_only.capabilities = imza::Capabilities::IMAGE;
    const std::string image_only_text
        = without_ansi(to_text(imza::model_picker_row(
            imza::make_model_row("c", "OpenAI", image_only), false)));
    CHECK(image_only_text.find("image") != std::string::npos);
    CHECK(image_only_text.find("pdf") == std::string::npos);
    CHECK(image_only_text.find("image · OpenAI") != std::string::npos);

    imza::ModelInfo unknown;
    unknown.id                 = "m2";
    const std::string untagged = without_ansi(to_text(imza::model_picker_row(
        imza::make_model_row("c", "OpenAI", unknown), false)));
    CHECK(untagged.find("image") == std::string::npos);
    CHECK(untagged.find("pdf") == std::string::npos);
    CHECK(untagged.find("·") == std::string::npos);
}

namespace {

// Braille patterns U+2800..U+28FF are UTF-8 E2 A0..A3 80..BF.
bool has_braille(const std::string& text)
{
    for (std::size_t i = 0; i + 2 < text.size(); ++i) {
        if (static_cast<unsigned char>(text[i]) == 0xE2
            && static_cast<unsigned char>(text[i + 1]) >= 0xA0
            && static_cast<unsigned char>(text[i + 1]) <= 0xA3) {
            return true;
        }
    }
    return false;
}

// Block elements U+2580..U+259F are UTF-8 E2 96 80..9F.
bool has_block(const std::string& text)
{
    for (std::size_t i = 0; i + 2 < text.size(); ++i) {
        if (static_cast<unsigned char>(text[i]) == 0xE2
            && static_cast<unsigned char>(text[i + 1]) == 0x96
            && static_cast<unsigned char>(text[i + 2]) >= 0x80
            && static_cast<unsigned char>(text[i + 2]) <= 0x9F) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("canvas line chart renders title, legend, and braille plot")
{
    imza::CanvasView line;
    line.kind   = imza::CanvasView::Kind::LINE;
    line.title  = "Traffic";
    line.series = { { "in", { 1.0, 5.0, 3.0 } }, { "out", { 2.0, 1.0, 4.0 } } };
    const std::string drawn
        = without_ansi(to_text(imza::canvas_chart(line, 60), 60, 30));
    CHECK(drawn.find("Traffic") != std::string::npos);
    CHECK(drawn.find("● in") != std::string::npos);
    CHECK(drawn.find("● out") != std::string::npos);
    CHECK(has_braille(drawn));
}

TEST_CASE("canvas bar chart renders category labels and blocks")
{
    imza::CanvasView bar;
    bar.kind   = imza::CanvasView::Kind::BAR;
    bar.title  = "Sales";
    bar.series = { { "mon", { 3.0 } }, { "tue", { 5.0 } }, { "wed", { 4.0 } } };
    const std::string drawn
        = without_ansi(to_text(imza::canvas_chart(bar, 60), 60, 30));
    CHECK(drawn.find("Sales") != std::string::npos);
    CHECK(drawn.find("mon") != std::string::npos);
    CHECK(drawn.find("tue") != std::string::npos);
    CHECK(drawn.find("wed") != std::string::npos);
    CHECK(has_block(drawn));
}

TEST_CASE("canvas pie chart renders a legend for every slice")
{
    imza::CanvasView pie;
    pie.kind   = imza::CanvasView::Kind::PIE;
    pie.series = { { "red", { 3.0 } }, { "green", { 1.0 } } };
    const std::string drawn
        = without_ansi(to_text(imza::canvas_chart(pie, 40), 60, 40));
    CHECK(drawn.find("● red") != std::string::npos);
    CHECK(drawn.find("● green") != std::string::npos);
    CHECK(has_braille(drawn));
}

TEST_CASE("canvas surface chart renders a wireframe")
{
    imza::CanvasView surface;
    surface.kind  = imza::CanvasView::Kind::SURFACE;
    surface.title = "Wave";
    surface.grid  = { { 0, 1, 0 }, { 1, 5, 1 }, { 0, 1, 0 } };
    const std::string drawn
        = without_ansi(to_text(imza::canvas_chart(surface, 60), 60, 40));
    CHECK(drawn.find("Wave") != std::string::npos);
    CHECK(has_braille(drawn));
}
