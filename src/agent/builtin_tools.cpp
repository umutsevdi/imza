#include "tools.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace ursa {

namespace {

    namespace fs = std::filesystem;

    constexpr std::size_t MAX_READ_LINES  = 2000;
    constexpr std::size_t MAX_LIST_ENTRIES = 2000;

    std::vector<std::string> split_lines(const std::string& text)
    {
        std::vector<std::string> lines;
        std::string line;
        for (const char c : text) {
            if (c == '\n') {
                lines.push_back(std::move(line));
                line.clear();
            } else {
                line += c;
            }
        }
        if (!line.empty()) {
            lines.push_back(std::move(line));
        }
        return lines;
    }

    std::string join_lines(const std::vector<std::string>& lines,
        std::size_t begin, std::size_t end)
    {
        std::string out;
        for (std::size_t i = begin; i <= end && i < lines.size(); ++i) {
            if (!out.empty()) {
                out += '\n';
            }
            out += lines[i];
        }
        return out;
    }

    ToolOutput error(std::string text)
    {
        return { ToolOutput::Kind::ERROR, std::move(text) };
    }

    std::string format_kb(std::uintmax_t bytes)
    {
        double kb = static_cast<double>(bytes) / 1024.0;
        std::ostringstream os;
        os << std::fixed << std::setprecision(1) << kb;
        std::string s = os.str();
        if (s.size() >= 2 && s.compare(s.size() - 2, 2, ".0") == 0) {
            s = s.substr(0, s.size() - 2);
        }
        return s + " KB";
    }

    ToolOutput read_run(const Json::Value& args)
    {
        if (!args.isObject() || !args["path"].isString()
            || args["path"].asString().empty()) {
            return error("read: 'path' must be a non-empty string");
        }
        const std::string path = args["path"].asString();

        std::error_code ec;
        const fs::path file(path);
        if (!fs::exists(file, ec)) {
            return error("read: no such file: " + path);
        }
        if (!fs::is_regular_file(file, ec)) {
            return error("read: not a file: " + path);
        }

        std::ifstream in(file, std::ios::binary);
        if (!in) {
            return error("read: cannot open: " + path);
        }
        const std::string content(
            (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (content.find('\0') != std::string::npos) {
            return error("read: binary file: " + path);
        }

        std::size_t begin = 1;
        if (args["line_begin"].isIntegral()) {
            const auto raw = args["line_begin"].asInt64();
            if (raw < 1) {
                return error("read: line_begin must be 1 or greater");
            }
            begin = static_cast<std::size_t>(raw);
        }
        bool end_given = false;
        std::size_t end = 0;
        if (args["line_end"].isIntegral()) {
            const auto raw = args["line_end"].asInt64();
            if (raw < 1) {
                return error("read: line_end must be 1 or greater");
            }
            end       = static_cast<std::size_t>(raw);
            end_given = true;
        }

        const std::vector<std::string> lines = split_lines(content);
        const std::size_t length             = lines.size();
        if (length == 0) {
            return { ToolOutput::Kind::OUTPUT, "(empty file)" };
        }
        if (begin > length) {
            return error("read: line_begin " + std::to_string(begin)
                + " exceeds file length " + std::to_string(length) + ": " + path);
        }
        if (end_given && end > length) {
            return error("read: line_end " + std::to_string(end)
                + " exceeds file length " + std::to_string(length) + ": " + path);
        }
        if (end_given && end < begin) {
            return error("read: line_end is before line_begin");
        }

        bool truncated = false;
        if (!end_given) {
            const std::size_t capped = begin + MAX_READ_LINES - 1;
            if (capped < length) {
                end       = capped;
                truncated = true;
            } else {
                end = length;
            }
        }

        std::string out = join_lines(lines, begin - 1, end - 1);
        if (truncated) {
            out += "\n\n[truncated: showing lines " + std::to_string(begin)
                + "-" + std::to_string(end) + " of "
                + std::to_string(length) + "]";
        }
        return { ToolOutput::Kind::OUTPUT, std::move(out) };
    }

    ToolOutput list_run(const Json::Value& args)
    {
        std::string dir = ".";
        if (args.isObject() && args["path"].isString()
            && !args["path"].asString().empty()) {
            dir = args["path"].asString();
        }

        std::error_code ec;
        const fs::path root(dir);
        if (!fs::exists(root, ec)) {
            return error("list: no such directory: " + dir);
        }
        if (!fs::is_directory(root, ec)) {
            return error("list: not a directory: " + dir);
        }

        std::vector<std::string> names;
        try {
            for (const auto& entry : fs::directory_iterator(root)) {
                std::string name = entry.path().filename().string();
                if (entry.is_directory(ec)) {
                    name += "/";
                }
                names.push_back(std::move(name));
            }
        } catch (const std::filesystem::filesystem_error& e) {
            return error(std::string("list: cannot read directory: ") + e.what());
        }
        std::sort(names.begin(), names.end());

        bool truncated = false;
        std::size_t count = names.size();
        if (count > MAX_LIST_ENTRIES) {
            names.resize(MAX_LIST_ENTRIES);
            truncated = true;
        }

    std::string out;
    for (const auto& name : names) {
        if (!out.empty()) {
            out += '\n';
        }
        out += name;
        if (name.empty() || name.back() == '/') {
            out += '\t' + std::string("—");
            continue;
        }
        std::error_code sec;
        const auto sz = fs::file_size(root / name, sec);
        out += '\t' + (sec ? "—" : format_kb(sz));
    }
        if (truncated) {
            out += "\n[truncated: showing first "
                + std::to_string(MAX_LIST_ENTRIES) + " of "
                + std::to_string(count) + " entries]";
        }
        return { ToolOutput::Kind::OUTPUT, std::move(out) };
    }

} // namespace

Tool make_read_tool()
{
    ToolSpec spec;
    spec.name        = "read";
    spec.description = "Read a text file and return its contents. Optionally "
                       "restrict output to a 1-based inclusive line range via "
                       "line_begin/line_end; without line_end, long files are "
                       "truncated.";
    spec.parameters = parse_json(
        R"json({"type":"object","properties":{"path":{"type":"string","description":"file path to read"},"line_begin":{"type":"integer","description":"first line to return (1-based, inclusive)"},"line_end":{"type":"integer","description":"last line to return (1-based, inclusive)"}},"required":["path"]})json");
    return { std::move(spec), read_run, ToolSafety::READ_ONLY };
}

Tool make_list_tool()
{
    ToolSpec spec;
    spec.name        = "list";
    spec.description = "List the entries of a directory (non-recursive), "
                       "sorted, one per line; directories carry a trailing "
                       "slash.";
    spec.parameters = parse_json(
        R"json({"type":"object","properties":{"path":{"type":"string","description":"directory to list (defaults to the current directory)"}}})json");
    return { std::move(spec), list_run, ToolSafety::READ_ONLY };
}

Tool make_ask_tool()
{
    ToolSpec spec;
    spec.name        = "ask";
    spec.description = "Ask the user one or more questions and wait for their "
                       "answers. Returns the answers as the tool result.";
    spec.parameters = parse_json(
        R"json({"type":"object","properties":{"questions":{"type":"array","description":"questions to ask (at least one)","minItems":1,"items":{"type":"object","properties":{"prompt":{"type":"string","description":"the question text"},"options":{"type":"array","items":{"type":"string"},"description":"selectable options (omit for free-text only)"},"multi":{"type":"boolean","description":"allow multiple option selections"},"free_text":{"type":"boolean","description":"allow a free-text answer in addition to options"}},"required":["prompt"]}}}},"required":["questions"]})json");
    return { std::move(spec), ToolHandler { }, ToolSafety::READ_ONLY };
}

ToolRegistry builtin_tools()
{
    ToolRegistry tools;
    tools.add(make_read_tool());
    tools.add(make_list_tool());
    tools.add(make_ask_tool());
    return tools;
}

} // namespace ursa
