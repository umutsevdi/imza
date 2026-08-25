#include <string>

#include <doctest/doctest.h>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include "ui.h"

namespace {

// Renders an element into a fixed-size screen and returns it as text.
std::string to_text(ftxui::Element element)
{
    using namespace ftxui;
    auto screen = Screen::Create(Dimension::Fixed(60), Dimension::Fixed(30));
    Render(screen, element);
    return screen.ToString();
}

} // namespace

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
