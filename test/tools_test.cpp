#include <doctest/doctest.h>
#include <json/json.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "tools.h"

namespace ursa {

namespace {

    std::vector<Tool> echo_tools()
    {
        return {
            { { "echo", "echo the message",
                  parse_json(
                      R"({"type":"object","properties":{"msg":{"type":"string"}}})") },
                [](const Json::Value& args) {
                    return ToolOutput { ToolOutput::Kind::OUTPUT,
                        args.get("msg", "").asString() };
                } }
        };
    }

    std::string read_file(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        std::ostringstream content;
        content << file.rdbuf();
        return content.str();
    }

    bool diff_has_right(const ToolOutput& out, std::string_view line)
    {
        if (!out.diff) {
            return false;
        }
        return std::any_of(out.diff->rows.begin(), out.diff->rows.end(),
            [&](const DiffRow& row) { return row.right == line; });
    }

} // namespace

TEST_CASE("tool helpers find and list specs")
{
    const std::vector<Tool> tools = echo_tools();
    REQUIRE(tools.size() == 1);

    const Tool* found = find_tool(tools, "echo");
    REQUIRE(found != nullptr);
    CHECK(found->spec.name == "echo");
    CHECK(found->spec.description == "echo the message");
    CHECK(found->spec.parameters["properties"].isMember("msg"));
    CHECK(found->safety == ToolSafety::MUTATING);
    CHECK(find_tool(tools, "missing") == nullptr);

    const auto specs = tool_specs(tools);
    REQUIRE(specs.size() == 1);
    CHECK(specs[0].name == "echo");
}

TEST_CASE("tool safety defaults to mutating and can be overridden")
{
    Tool mutating;
    mutating.spec.name = "m";
    CHECK(mutating.safety == ToolSafety::MUTATING);

    Tool read_only { { "ro", "", Json::Value(Json::objectValue) },
        [](const Json::Value&) {
            return ToolOutput { ToolOutput::Kind::OUTPUT, "" };
        },
        ToolSafety::READ_ONLY };
    CHECK(read_only.safety == ToolSafety::READ_ONLY);
}

TEST_CASE("dispatch parses object args for the handler")
{
    const std::vector<Tool> tools = echo_tools();
    const ToolOutput out          = dispatch_tool(
        tools, ToolCallRequest { "echo", R"({"msg":"hi"})", "", "" });
    CHECK(out.kind == ToolOutput::Kind::OUTPUT);
    CHECK(out.text == "hi");
}

TEST_CASE("dispatch passes non-JSON args through as a string value")
{
    std::vector<Tool> tools {
        { { "raw", "takes raw text", Json::Value(Json::objectValue) },
            [](const Json::Value& args) {
                return ToolOutput { ToolOutput::Kind::OUTPUT, args.asString() };
            } }
    };
    const ToolOutput out = dispatch_tool(tools, { "raw", "ls -la", "", "" });
    CHECK(out.kind == ToolOutput::Kind::OUTPUT);
    CHECK(out.text == "ls -la");
}

TEST_CASE("dispatch reports unknown tools as errors")
{
    const std::vector<Tool> tools;
    const ToolOutput out = dispatch_tool(tools, { "nope", "{}", "", "" });
    CHECK(out.kind == ToolOutput::Kind::ERROR);
    CHECK(out.text.find("unknown tool: nope") != std::string::npos);
}

TEST_CASE("dispatch propagates handler errors")
{
    std::vector<Tool> tools {
        { { "boom", "always fails", Json::Value(Json::objectValue) },
            [](const Json::Value&) {
                return ToolOutput { ToolOutput::Kind::ERROR, "it broke" };
            } }
    };
    const ToolOutput out = dispatch_tool(tools, { "boom", "{}", "", "" });
    CHECK(out.kind == ToolOutput::Kind::ERROR);
    CHECK(out.text == "it broke");
}

TEST_CASE("edit produces a diff whose right side holds the new content")
{
    const std::filesystem::path path
        = std::filesystem::temp_directory_path() / "ursa_edit_diff_test.txt";
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << "original line\nappended line\n";
    }

    std::vector<Tool> tools { make_edit_tool() };
    const ToolOutput out = dispatch_tool(tools,
        ToolCallRequest { "edit",
            R"({"file_path":")" + path.string()
                + R"(","old_string":"original line","new_string":"edited line"})",
            "", "" });

    CHECK(out.kind == ToolOutput::Kind::OUTPUT);
    REQUIRE(out.diff.has_value());

    bool saw_new          = false;
    bool saw_old_on_right = false;
    for (const auto& row : out.diff->rows) {
        if (row.right.find("edited line") != std::string::npos) {
            saw_new = true;
        }
        if (row.right == "original line") {
            saw_old_on_right = true;
        }
    }
    CHECK(saw_new);
    CHECK_FALSE(saw_old_on_right);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("edit preserves text around partial-line replacements")
{
    const auto path
        = std::filesystem::temp_directory_path() / "ursa_edit_partial_test.txt";
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "prefix old suffix\nsecond line\n";
    }
    const Tool tool      = make_edit_tool();
    const ToolOutput out = tool.run(parse_json(std::string(R"({"file_path":")")
        + path.string() + R"(","old_string":"old","new_string":"new"})"));

    REQUIRE(out.kind == ToolOutput::Kind::OUTPUT);
    CHECK(read_file(path) == "prefix new suffix\nsecond line\n");
    CHECK(diff_has_right(out, "prefix new suffix"));

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("edit preserves gaps while replacing multiple occurrences")
{
    const auto path = std::filesystem::temp_directory_path()
        / "ursa_edit_multiple_test.txt";
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "old middle old tail\n";
    }
    const Tool tool = make_edit_tool();
    const ToolOutput out
        = tool.run(parse_json(std::string(R"({"file_path":")") + path.string()
            + R"(","old_string":"old","new_string":"new","replace_count":0})"));

    REQUIRE(out.kind == ToolOutput::Kind::OUTPUT);
    CHECK(read_file(path) == "new middle new tail\n");
    CHECK(diff_has_right(out, "new middle new tail"));

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("write replaces an inclusive line range and reports final content")
{
    const auto path
        = std::filesystem::temp_directory_path() / "ursa_write_range_test.txt";
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "one\ntwo\nthree\nfour\n";
    }
    const Tool tool      = make_write_tool();
    const ToolOutput out = tool.run(parse_json(std::string(R"({"file_path":")")
        + path.string()
        + R"(","text":"replacement","overwrite":true,"line_begin":2,"line_end":3})"));

    REQUIRE(out.kind == ToolOutput::Kind::OUTPUT);
    CHECK(read_file(path) == "one\nreplacement\nfour\n");
    CHECK(diff_has_right(out, "replacement"));
    CHECK_FALSE(diff_has_right(out, "two"));
    CHECK_FALSE(diff_has_right(out, "three"));

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("write line_end zero replaces through the end")
{
    const auto path
        = std::filesystem::temp_directory_path() / "ursa_write_to_end_test.txt";
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "one\ntwo\nthree\n";
    }
    const Tool tool      = make_write_tool();
    const ToolOutput out = tool.run(parse_json(std::string(R"({"file_path":")")
        + path.string()
        + R"(","text":"last","overwrite":true,"line_begin":2,"line_end":0})"));

    REQUIRE(out.kind == ToolOutput::Kind::OUTPUT);
    CHECK(read_file(path) == "one\nlast\n");
    CHECK(diff_has_right(out, "last"));

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

} // namespace ursa
