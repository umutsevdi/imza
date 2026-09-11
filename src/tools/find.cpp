#include "tools/tool.h"

#include "network/json_io.h"
#include "platform/command_runner.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace imza {

namespace {

#ifdef _WIN32
    bool unsafe_command_value(std::string_view value)
    {
        return value.find_first_of("\"%\r\n") != std::string_view::npos;
    }

    std::string command_argument(std::string_view value)
    {
        std::string quoted = "\"" + std::string(value);
        if (!value.empty() && value.back() == '\\') {
            quoted += '\\';
        }
        return quoted + "\"";
    }

    std::string powershell_literal(std::string_view value)
    {
        std::string escaped;
        for (const char character : value) {
            escaped += character == '\'' ? "''" : std::string(1, character);
        }
        return "'" + escaped + "'";
    }
#endif

    std::string find_command(
        std::string_view pattern, std::string_view path, bool has_rg)
    {
        if (has_rg) {
#ifdef _WIN32
            return "rg -n -- " + command_argument(pattern) + " "
                + command_argument(path);
#else
            return "rg -n -- " + shell_quote(std::filesystem::path(pattern))
                + " " + shell_quote(std::filesystem::path(path));
#endif
        }
#ifdef _WIN32
        return "powershell.exe -NoProfile -NonInteractive -Command \""
               "Get-ChildItem -LiteralPath "
            + powershell_literal(path)
            + " -Recurse -File | Select-String -Pattern "
            + powershell_literal(pattern) + "\"";
#else
        return "grep -rsnE -- " + shell_quote(std::filesystem::path(pattern))
            + " " + shell_quote(std::filesystem::path(path));
#endif
    }

    ToolOutput find_run(const Json::Value& args, bool has_rg)
    {
        if (const auto validation
            = validate_filesystem_tool_arguments("find", args)) {
            return tool_error(*validation);
        }
        const std::string pattern = args["pattern"].asString();
        const std::string path
            = args["path"].isString() && !args["path"].asString().empty()
            ? args["path"].asString()
            : ".";
#ifdef _WIN32
        if (unsafe_command_value(pattern) || unsafe_command_value(path)) {
            return tool_error(
                "find: pattern or path contains unsupported characters");
        }
#endif

        constexpr auto timeout = std::chrono::seconds { 10 };
        CommandResult result
            = run_command(find_command(pattern, path, has_rg), timeout);
        if (!result.spawned) {
            return tool_error("find: failed to start search command");
        }
        if (result.timed_out) {
            return tool_error("find: search timed out after 10 seconds");
        }
        if (result.exit_code > 1) {
            return tool_error(result.output.empty()
                    ? "find: search command failed with exit code "
                        + std::to_string(result.exit_code)
                    : "find: " + std::move(result.output));
        }
        while (!result.output.empty()
            && (result.output.back() == '\n' || result.output.back() == '\r')) {
            result.output.pop_back();
        }
        return { ToolOutput::Kind::OUTPUT, std::move(result.output) };
    }

} // namespace

Tool make_find_tool(bool has_rg)
{
    ToolSpec spec;
    spec.name = "find";
    spec.description
        = "Search a file or directory recursively for a regular expression "
          "and return matching lines with line numbers. The path defaults to "
          "the current directory.";
    spec.parameters = parse_json(
        R"json({"type":"object","properties":{"pattern":{"type":"string","description":"regular expression to search for"},"path":{"type":"string","description":"file or directory to search (defaults to the current directory)"}},"required":["pattern"]})json");
    return { std::move(spec),
        [has_rg](const Json::Value& args) { return find_run(args, has_rg); } };
}

} // namespace imza
