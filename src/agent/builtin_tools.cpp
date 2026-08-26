#include "tools.h"

#include "command.h"
#include "util.h"

#include <algorithm>
#include <chrono>
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
        }        if (begin > length) {
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

    ToolOutput shell_run(const Json::Value& args)
    {
        if (!args.isObject() || !args["command"].isString()
            || args["command"].asString().empty()) {
            return error("shell: 'command' must be a non-empty string");
        }
        const std::string command = args["command"].asString();

        std::chrono::seconds timeout = std::chrono::seconds(10);
        if (args["timeout"].isIntegral()) {
            const auto raw = args["timeout"].asInt64();
            if (raw < 1) {
                return error("shell: timeout must be 1 or greater");
            }
            timeout = std::chrono::seconds(static_cast<long>(raw));
        }

        const CommandResult r = run_command(command, timeout);
        if (!r.spawned) {
            return error("shell: failed to execute command");
        }

        std::string out = command + "\n";
        out += r.output;
        if (!out.empty() && out.back() != '\n') {
            out += '\n';
        }
        if (r.timed_out) {
            out += "[command timed out after "
                + std::to_string(timeout.count()) + "s]\n";
        } else {
            out += "[exit code: " + std::to_string(r.exit_code) + "]\n";
        }
        return { ToolOutput::Kind::OUTPUT, std::move(out) };
    }

    bool load_text(const std::string& path, std::string& out, std::string& err)
    {
        std::error_code ec;
        const fs::path file(path);
        if (!fs::exists(file, ec)) {
            err = "no such file: " + path;
            return false;
        }
        if (!fs::is_regular_file(file, ec)) {
            err = "not a file: " + path;
            return false;
        }
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            err = "cannot open: " + path;
            return false;
        }
        const std::string content(
            (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (content.find('\0') != std::string::npos) {
            err = "binary file: " + path;
            return false;
        }
        out = content;
        return true;
    }

    bool save_text(const std::string& path, const std::string& content,
        std::string& err)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            err = "cannot write: " + path;
            return false;
        }
        out << content;
        return true;
    }

    std::string rebuild_lines(const std::vector<std::string>& lines,
        bool trailing_newline)
    {
        std::string out;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (i != 0) {
                out += '\n';
            }
            out += lines[i];
        }
        if (!lines.empty() && trailing_newline) {
            out += '\n';
        }
        return out;
    }

    struct EditSpan {
        std::size_t old_begin;
        std::size_t old_end;
        std::size_t new_begin;
        std::size_t new_end;
    };

    std::size_t line_of_offset(const std::vector<std::string>& lines,
        std::size_t off)
    {
        std::size_t pos = 0;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const std::size_t len = lines[i].size();
            if (off < pos + len) {
                return i;
            }
            pos += len;
            if (i + 1 < lines.size()) {
                if (off == pos) {
                    return i + 1;
                }
                ++pos;
            }
        }
        return lines.size();
    }

    long get_int(const Json::Value& v, long def)
    {
        if (v.isIntegral()) {
            return v.asInt64();
        }
        if (v.isString()) {
            try {
                return std::stol(v.asString());
            } catch (...) {
                return def;
            }
        }
        return def;
    }

    DiffView build_diff_view(const std::string& path,
        const std::vector<std::string>& old_lines,
        const std::vector<std::string>& new_lines,
        const std::vector<EditSpan>& edits, std::size_t context = 3)
    {
        DiffView dv;
        dv.file = path;
        if (old_lines.empty() && new_lines.empty()) {
            return dv;
        }
        const auto push_same = [&](std::size_t o0, std::size_t o1,
                                     std::size_t n0) {
            const std::size_t len = o1 - o0;
            for (std::size_t k = 0; k < len; ++k) {
                DiffRow r;
                r.kind     = DiffRow::Kind::SAME;
                r.left_no  = o0 + k + 1;
                r.right_no = n0 + k + 1;
                r.left     = old_lines[o0 + k];
                r.right    = new_lines[n0 + k];
                dv.rows.push_back(std::move(r));
            }
        };
        const auto push_skip = [&](std::size_t count, std::size_t lo,
                                     std::size_t ln) {
            DiffRow r;
            r.kind     = DiffRow::Kind::SAME;
            r.left_no  = lo;
            r.right_no = ln;
            r.left     = "… " + std::to_string(count) + " unchanged line(s) …";
            r.right    = r.left;
            dv.rows.push_back(std::move(r));
        };
        const auto push_edit = [&](const EditSpan& ed) {
            const std::size_t olen = ed.old_end - ed.old_begin;
            const std::size_t nlen = ed.new_end - ed.new_begin;
            const std::size_t len  = std::max(olen, nlen);
            for (std::size_t k = 0; k < len; ++k) {
                DiffRow r;
                if (k < olen && k < nlen) {
                    r.kind = DiffRow::Kind::ADD;
                } else if (k < olen) {
                    r.kind = DiffRow::Kind::REMOVE;
                } else {
                    r.kind = DiffRow::Kind::ADD;
                }
                if (k < olen) {
                    r.left_no = ed.old_begin + k + 1;
                    r.left    = old_lines[ed.old_begin + k];
                }
                if (k < nlen) {
                    r.right_no = ed.new_begin + k + 1;
                    r.right    = new_lines[ed.new_begin + k];
                }
                dv.rows.push_back(std::move(r));
            }
        };

        std::size_t oi = 0;
        std::size_t ni = 0;
        for (std::size_t e = 0; e < edits.size(); ++e) {
            const EditSpan& ed = edits[e];
            const std::size_t gap_len = ed.old_begin - oi;
            if (gap_len > 0) {
                const std::size_t take   = std::min(gap_len, context);
                const std::size_t start  = ed.old_begin - take;
                if (gap_len > context) {
                    push_skip(gap_len - context, oi + 1, ni + 1);
                }
                push_same(start, ed.old_begin, ni + (start - oi));
            }
            push_edit(ed);
            oi = ed.old_end;
            ni = ed.new_end;
        }
        const std::size_t tail_len = old_lines.size() - oi;
        if (tail_len > 0) {
            const std::size_t take = std::min(tail_len, context);
            push_same(oi, oi + take, ni);
            if (tail_len > context) {
                push_skip(tail_len - context, oi + take + 1, ni + take + 1);
            }
        }
        return dv;
    }

    ToolOutput make_diff_result(std::string summary,
        const std::string& path, const std::vector<std::string>& old_lines,
        const std::vector<std::string>& new_lines,
        const std::vector<EditSpan>& edits)
    {
        ToolOutput out {
            ToolOutput::Kind::OUTPUT, std::move(summary) };
        out.diff = build_diff_view(path, old_lines, new_lines, edits);
        return out;
    }

    ToolOutput edit_run(const Json::Value& args)
    {
        if (!args.isObject() || !args["file_path"].isString()
            || args["file_path"].asString().empty()) {
            return error("edit: 'file_path' must be a non-empty string");
        }
        const std::string path = args["file_path"].asString();
        if (!args["old_string"].isString()
            || args["old_string"].asString().empty()) {
            return error("edit: 'old_string' must be a non-empty string");
        }
        const std::string old = args["old_string"].asString();
        const std::string fresh
            = args["new_string"].isString() ? args["new_string"].asString() : "";

        long replace_count = get_int(args["replace_count"], 1);
        if (replace_count < 0) {
            return error("edit: replace_count must be 0 or greater");
        }
        long offset = get_int(args["offset"], 0);
        if (offset < 0) {
            return error("edit: offset must be 0 or greater");
        }

        std::string content, err;
        if (!load_text(path, content, err)) {
            return error("edit: " + err);
        }

        std::size_t start = 0;
        if (offset > 0) {
            std::size_t line = 1;
            std::size_t i     = 0;
            const std::size_t n = content.size();
            while (i < n && line < static_cast<std::size_t>(offset)) {
                if (content[i] == '\n') {
                    ++line;
                }
                ++i;
            }
            start = i;
        }

        std::vector<std::size_t> matches;
        std::size_t p = start;
        while ((p = content.find(old, p)) != std::string::npos) {
            matches.push_back(p);
            p += old.size();
        }
        if (matches.empty()) {
            return error(offset > 0
                ? "edit: old_string not found after offset line"
                : "edit: old_string not found");
        }

        const std::size_t n = replace_count == 0
            ? matches.size()
            : std::min(static_cast<std::size_t>(replace_count), matches.size());

        const std::vector<std::string> old_lines = split_lines(content);
        std::vector<std::string> new_lines;
        new_lines.reserve(old_lines.size());
        std::vector<EditSpan> edits;
        std::size_t consumed = 0;
        std::size_t replaced = 0;
        std::string out;
        out.reserve(content.size());
        std::size_t cursor = 0;
        for (std::size_t idx = 0; idx < matches.size(); ++idx) {
            const std::size_t match    = matches[idx];
            const bool do_replace       = idx < n;
            const std::size_t m_end     = match + old.size();
            const std::size_t l_start   = line_of_offset(old_lines, match);
            const std::size_t l_end     = line_of_offset(old_lines, m_end);
            for (; consumed < l_start && consumed < old_lines.size();
                ++consumed) {
                new_lines.push_back(old_lines[consumed]);
            }
            if (do_replace) {
                const std::size_t nb = new_lines.size();
                const auto repl      = split_lines(fresh);
                for (const auto& rl : repl) {
                    new_lines.push_back(rl);
                }
                edits.push_back(
                    { l_start, l_end, nb, new_lines.size() });
                consumed = l_end;
                out += fresh;
                ++replaced;
            } else {
                for (; consumed < l_end && consumed < old_lines.size();
                    ++consumed) {
                    new_lines.push_back(old_lines[consumed]);
                }
                out += old;
            }
            cursor = m_end;
        }
        out += content.substr(cursor);
        for (; consumed < old_lines.size(); ++consumed) {
            new_lines.push_back(old_lines[consumed]);
        }

        if (!save_text(path, out, err)) {
            return error("edit: " + err);
        }
        return make_diff_result("edit: replaced "
                + std::to_string(replaced) + " occurrence(s) in " + path,
            path, old_lines, new_lines, edits);
    }

    ToolOutput write_run(const Json::Value& args)
    {
        if (!args.isObject() || !args["file_path"].isString()
            || args["file_path"].asString().empty()) {
            return error("write: 'file_path' must be a non-empty string");
        }
        const std::string path = args["file_path"].asString();
        if (!args["text"].isString()) {
            return error("write: 'text' must be a string");
        }
        const std::string text = args["text"].asString();

        const bool overwrite
            = args["overwrite"].isBool() ? args["overwrite"].asBool() : false;

        std::error_code ec;
        const bool exists = fs::exists(fs::path(path), ec);

        std::string content, err;
        if (exists) {
            if (!load_text(path, content, err)) {
                return error("write: " + err);
            }
        } else {
            if (overwrite) {
                return error("write: no such file: " + path);
            }
            content.clear();
        }

        const bool trailing_newline
            = content.empty() || content.back() == '\n';
        const std::vector<std::string> old_lines = split_lines(content);
        const std::vector<std::string> insert    = split_lines(text);
        std::vector<std::string> new_lines       = old_lines;
        std::vector<EditSpan> edits;

        if (!overwrite) {
            const long line = get_int(args["line"], 0);
            if (line < 0) {
                return error("write: line must be 0 or greater");
            }
            std::size_t at = static_cast<std::size_t>(line);
            if (at > old_lines.size()) {
                at = old_lines.size();
            }
            new_lines.insert(
                new_lines.begin() + static_cast<std::ptrdiff_t>(at),
                insert.begin(), insert.end());
            edits.push_back({ at, at, at, at + insert.size() });
            return make_diff_result("write: inserted text below line "
                    + std::to_string(line) + " in " + path,
                path, old_lines, new_lines, edits);
        }

        if (!args["line_begin"].isIntegral() && !args["line_begin"].isString()) {
            return error("write: block-replace requires line_begin and line_end");
        }
        if (!args["line_end"].isIntegral() && !args["line_end"].isString()) {
            return error("write: block-replace requires line_begin and line_end");
        }
        const long lb = get_int(args["line_begin"], 0);
        const long le = get_int(args["line_end"], 0);
        if (lb < 0 || le < 0) {
            return error("write: line_begin/line_end must be 0 or greater");
        }
        if (le < lb) {
            return error("write: line_end is before line_begin");
        }

        std::size_t begin_idx
            = lb == 0 ? 0 : static_cast<std::size_t>(lb - 1);
        std::size_t end_incl
            = le == 0 ? old_lines.size() : static_cast<std::size_t>(le - 1);
        if (begin_idx > old_lines.size()) {
            begin_idx = old_lines.size();
        }
        if (end_incl > old_lines.size()) {
            end_incl = old_lines.size();
        }
        if (end_incl < begin_idx) {
            end_incl = begin_idx;
        }

        new_lines.erase(
            new_lines.begin() + static_cast<std::ptrdiff_t>(begin_idx),
            new_lines.begin() + static_cast<std::ptrdiff_t>(end_incl));
        new_lines.insert(
            new_lines.begin() + static_cast<std::ptrdiff_t>(begin_idx),
            insert.begin(), insert.end());
        edits.push_back({ begin_idx, end_incl, begin_idx,
            begin_idx + insert.size() });

        const std::string out = rebuild_lines(new_lines, trailing_newline);
        if (!save_text(path, out, err)) {
            return error("write: " + err);
        }
        return make_diff_result("write: replaced lines "
                + std::to_string(lb) + "-" + std::to_string(le) + " in " + path,
            path, old_lines, new_lines, edits);
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

Tool make_shell_tool()
{
    ToolSpec spec;
    spec.name = "shell";
    spec.description = "Run a single shell command (one-liner) on the host and "
                       "return its combined stdout/stderr. The command is subject "
                       "to a timeout (seconds, default 10) after which it is "
                       "terminated.";
    spec.parameters = parse_json(
        R"json({"type":"object","properties":{"command":{"type":"string","description":"the shell command to run"},"timeout":{"type":"integer","description":"maximum runtime in seconds before the command is killed (default 10)"}},"required":["command"]})json");
    return { std::move(spec), shell_run, ToolSafety::MUTATING, false };
}

Tool make_todo_tool()
{
    ToolSpec spec;
    spec.name        = "todo";
    spec.description = "Create and maintain a structured task list for the "
                       "current coding session. Tracks progress, organizes "
                       "multi-step work, and surfaces status to the user. Pass "
                       "the complete updated list each time; it replaces the "
                       "previous one.";
    spec.parameters = parse_json(
        R"json({"type":"object","properties":{"todos":{"type":"array","description":"the updated todo list","items":{"type":"object","properties":{"content":{"type":"string","description":"short imperative description of the task"},"status":{"type":"string","enum":["pending","in_progress","completed","cancelled"],"description":"task state (default pending)"}},"required":["content"]}}},"required":["todos"]})json");
    return { std::move(spec), ToolHandler { }, ToolSafety::READ_ONLY };
}

Tool make_edit_tool()
{
    ToolSpec spec;
    spec.name = "edit";
    spec.description = "Replace text in a file by pattern match. Replaces the "
                       "first N occurrences of old_string with new_string "
                       "(replace_count, default 1; 0 means all). Matching can be "
                       "limited to start at a given 1-based line via offset "
                       "(default 0 = whole file). The file must already exist.";
    spec.parameters = parse_json(
        R"json({"type":"object","properties":{"file_path":{"type":"string","description":"file to edit"},"old_string":{"type":"string","description":"existing text to match and replace (non-empty)"},"new_string":{"type":"string","description":"replacement text (empty string deletes the match)"},"replace_count":{"type":"integer","description":"replace the first N matches (default 1; 0 = all)"},"offset":{"type":"integer","description":"1-based line to start matching from (default 0 = whole file)"}},"required":["file_path","old_string","new_string"]})json");
    return { std::move(spec), edit_run, ToolSafety::MUTATING };
}

Tool make_write_tool()
{
    ToolSpec spec;
    spec.name = "write";
    spec.description = "Add text to a file by line. In insert mode (default), "
                       "text is inserted below the 1-based line given by 'line' "
                       "(0 = prepend, >=last = append). In block-replace mode "
                       "(overwrite=true), the inclusive line range "
                       "line_begin..line_end is replaced with text (line_begin 0 "
                       "= from start, line_end 0 = to end). Missing files are "
                       "created in insert mode.";
    spec.parameters = parse_json(
        R"json({"type":"object","properties":{"file_path":{"type":"string","description":"file to write to"},"text":{"type":"string","description":"text to insert or write"},"line":{"type":"integer","description":"insert below this 1-based line (insert mode only)"},"overwrite":{"type":"boolean","description":"replace a line range instead of inserting (default false)"},"line_begin":{"type":"integer","description":"first line of the range to replace (block mode)"},"line_end":{"type":"integer","description":"last line of the range to replace, inclusive (block mode)"}},"required":["file_path","text"]})json");
    return { std::move(spec), write_run, ToolSafety::MUTATING };
}

ToolRegistry builtin_tools()
{
    ToolRegistry tools;
    tools.add(make_read_tool());
    tools.add(make_list_tool());
    tools.add(make_ask_tool());
    tools.add(make_shell_tool());
    tools.add(make_todo_tool());
    tools.add(make_edit_tool());
    tools.add(make_write_tool());
    return tools;
}

} // namespace ursa
