#include <string>

#include <doctest/doctest.h>
#include <ftxui/component/component.hpp>

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
