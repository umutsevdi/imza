#include <doctest/doctest.h>

#include <string>

#include "common/util.h"
#include "network/json_io.h"
#include "network/web.h"
#include "tools/tool.h"

TEST_CASE("normalize_web_url upgrades http and rejects bad schemes")
{
    std::string out;

    REQUIRE(imza::normalize_web_url("http://example.com/a", out)
        == imza::Status::OK);
    CHECK(out == "https://example.com/a");

    REQUIRE(imza::normalize_web_url("  HTTPS://Example.COM/x  ", out)
        == imza::Status::OK);
    CHECK(out == "HTTPS://Example.COM/x");

    REQUIRE(imza::normalize_web_url("https://example.com", out)
        == imza::Status::OK);
    CHECK(out == "https://example.com");

    CHECK(imza::normalize_web_url("ftp://example.com", out)
        == imza::Status::INVALID_URL);
    CHECK(imza::normalize_web_url("file:///etc/passwd", out)
        == imza::Status::INVALID_URL);
    CHECK(imza::normalize_web_url("example.com", out)
        == imza::Status::INVALID_URL);
    CHECK(imza::normalize_web_url("", out) == imza::Status::INVALID_URL);
}

TEST_CASE("truncate_utf8 keeps complete sequences")
{
    const std::string two_byte   = "a"
                                   "\xc3"
                                   "\xa9"
                                   "b";
    const std::string three_byte = "\xe6"
                                   "\x97"
                                   "\xa5";
    const std::string four_byte  = "\xf0"
                                   "\x9f"
                                   "\x98"
                                   "\x80";

    CHECK(imza::truncate_utf8(two_byte, 10) == two_byte);
    CHECK(imza::truncate_utf8(two_byte, 1) == "a");
    CHECK(imza::truncate_utf8(two_byte, 2) == "a");
    CHECK(imza::truncate_utf8(two_byte, 3)
        == "a"
           "\xc3"
           "\xa9");
    CHECK(imza::truncate_utf8(three_byte + "x", 3) == three_byte);
    CHECK(imza::truncate_utf8(three_byte + "x", 2) == "");
    CHECK(imza::truncate_utf8(four_byte, 4) == four_byte);
    CHECK(imza::truncate_utf8(four_byte + four_byte, 7) == four_byte);
    CHECK(imza::truncate_utf8("plain", 3) == "pla");
    CHECK(imza::truncate_utf8("", 5) == "");
}

TEST_CASE("html_to_text strips markup and decodes entities")
{
    const std::string html
        = "<html><head><title>T</title><style>.x{color:red}</style></head>\n"
          "<body><h1>Hello</h1><p>World &amp; more &#39;quoted&#39; text</p>\n"
          "<script>var a = 1 < 2;</script>\n"
          "<!-- hidden comment -->\n"
          "<ul><li>one</li><li>two</li></ul>\n"
          "<a href=\"x\">link text</a></body></html>";

    CHECK(imza::html_to_text(html)
        == "Hello\nWorld & more 'quoted' text\none\ntwo\nlink text");
}

TEST_CASE("html_to_text leaves unknown entities and text intact")
{
    CHECK(imza::html_to_text("a &foo; b") == "a &foo; b");
    CHECK(imza::html_to_text("<span>just</span> text") == "just text");
    CHECK(imza::html_to_text("<p>line<br>break</p>") == "line\nbreak");
    CHECK(imza::html_to_text("&#65;&#x42;") == "AB");
    CHECK(imza::html_to_text("") == "");
    CHECK(imza::html_to_text("plain words") == "plain words");
}

TEST_CASE("html_to_text rejects surrogate code points")
{
    CHECK(imza::html_to_text("&#xD800;") == "&#xD800;");
    CHECK(imza::html_to_text("&#xdc00;") == "&#xdc00;");
    CHECK(imza::html_to_text("&#xDFFF;") == "&#xDFFF;");
    CHECK(imza::html_to_text("&#xD7FF;") != "&#xD7FF;");
    CHECK(imza::html_to_text("<p>&#xD800; ok</p>") == "&#xD800; ok");
}

TEST_CASE("mcp_search_text reads JSON and SSE responses")
{
    const std::string json_response
        = R"({"jsonrpc":"2.0","id":1,"result":)"
          R"({"content":[{"type":"text","text":"1. Example Result"}]}})";
    CHECK(imza::mcp_search_text(json_response) == "1. Example Result");

    const std::string sse_response
        = "event: message\n"
          "data: {\"result\":{\"content\":[{\"type\":\"text\","
          "\"text\":\"sse text\"}]}}\n\n";
    CHECK(imza::mcp_search_text(sse_response) == "sse text");

    CHECK(imza::mcp_search_text(
              R"({"jsonrpc":"2.0","id":1,"error":{"code":-32000}})")
        == "");
    CHECK(
        imza::mcp_search_text(R"({"result":{"content":[{"text":""}]}})") == "");
    CHECK(imza::mcp_search_text("totally not json") == "");
}

TEST_CASE("webfetch rejects invalid arguments")
{
    const auto tool = imza::make_webfetch_tool();

    const auto missing = tool.run(imza::parse_json("{}"));
    CHECK(missing.kind == imza::ToolOutput::Kind::ERROR);

    const auto empty = tool.run(imza::parse_json(R"({"url":""})"));
    CHECK(empty.kind == imza::ToolOutput::Kind::ERROR);

    const auto scheme
        = tool.run(imza::parse_json(R"({"url":"ftp://example.com"})"));
    CHECK(scheme.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(scheme.text.starts_with("webfetch:"));
}

TEST_CASE("websearch rejects invalid arguments")
{
    const auto tool = imza::make_websearch_tool();

    const auto missing = tool.run(imza::parse_json("{}"));
    CHECK(missing.kind == imza::ToolOutput::Kind::ERROR);

    const auto empty = tool.run(imza::parse_json(R"({"query":""})"));
    CHECK(empty.kind == imza::ToolOutput::Kind::ERROR);
    CHECK(empty.text.starts_with("websearch:"));
}

TEST_CASE("web tools are registered")
{
    const auto tools = imza::default_tools();

    const auto* fetch = imza::find_tool(tools, "webfetch");
    REQUIRE(fetch != nullptr);

    const auto* search = imza::find_tool(tools, "websearch");
    REQUIRE(search != nullptr);
}
