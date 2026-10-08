#include "tools/bindings.h"

#include "common/util.h"
#include "permissions/filesystem.h"
#include "tools/file_ops.h"

#include <tree_sitter/api.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <locale>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace imza {

#include "language_registry.inc"

namespace {

    namespace fs = std::filesystem;

    constexpr int MAX_LIST_ENTRIES = 2000;
    constexpr int MAX_GREP_ROWS    = 500;
    constexpr int MAX_LIST_DEPTH   = 5;

    int binding_read(lua_State* L)
    {
        const std::string path                 = luaL_checkstring(L, 1);
        const std::optional<lua_Integer> first = opt_integer(L, 2);
        const std::optional<lua_Integer> last  = opt_integer(L, 3);
        const lua_Integer first_line           = first.value_or(1);
        if (first_line < 1 || (last.has_value() && *last < first_line)) {
            return binding_error(L,
                "read: line range must be 1-based and last_line >= first_line");
        }

        const ReadFileRequest request { path,
            static_cast<std::size_t>(first_line),
            last.has_value()
                ? std::optional<std::size_t>(static_cast<std::size_t>(*last))
                : std::nullopt };
        std::string target;
        if (const int denied = authorize_target(L, request, path, target)) {
            return denied;
        }

        // load_text rejects binary files, so any read range agrees with edit.
        std::string err;
        std::string content;
        if (!load_text(target, content, err)) {
            return binding_error(
                L, "read: " + err + " (requested " + path + ")");
        }
        const std::vector<std::string> lines = split_lines(content);
        if (lines.empty()) {
            lua_pushlstring(L, "", 0);
            return 1;
        }
        if (static_cast<std::size_t>(first_line) > lines.size()) {
            return binding_error(L,
                "read: first_line " + std::to_string(first_line)
                    + " exceeds file length " + std::to_string(lines.size())
                    + ": " + path);
        }
        const auto first_row
            = lines.begin() + static_cast<std::ptrdiff_t>(first_line - 1);
        const auto last_row = last.has_value() ? lines.begin()
                + static_cast<std::ptrdiff_t>(std::min<std::size_t>(
                    static_cast<std::size_t>(*last), lines.size()))
                                               : lines.end();
        std::string out
            = join_lines(std::vector<std::string>(first_row, last_row), true);
        out = truncate_marked(std::move(out), MAX_OUTPUT_BYTES);
        lua_pushlstring(L, out.data(), out.size());
        return 1;
    }

    std::string format_kb(std::uintmax_t bytes)
    {
        std::ostringstream os;
        os << std::fixed << std::setprecision(1)
           << static_cast<double>(bytes) / 1024.0;
        std::string s = os.str();
        if (s.size() >= 2 && s.compare(s.size() - 2, 2, ".0") == 0) {
            s = s.substr(0, s.size() - 2);
        }
        return s + " KB";
    }

    void push_list_entry(lua_State* L, const fs::directory_entry& entry,
        const fs::path& root, std::error_code& ec)
    {
        lua_newtable(L);
        std::error_code rec;
        const std::string name = fs::relative(entry.path(), root, rec).string();
        lua_pushlstring(L, name.data(), name.size());
        lua_setfield(L, -2, "path");
        const bool directory = entry.is_directory(ec);
        if (directory) {
            lua_pushliteral(L, "dir");
        } else {
            lua_pushliteral(L, "file");
        }
        lua_setfield(L, -2, "type");
        if (!directory) {
            const auto size      = entry.file_size(ec);
            const std::string kb = ec ? "-" : format_kb(size);
            lua_pushlstring(L, kb.data(), kb.size());
            lua_setfield(L, -2, "size");
        }
    }

    int list_directory(lua_State* L, const fs::path& root,
        const std::string& target, int depth, bool show_hidden, int* count)
    {
        std::error_code ec;
        fs::directory_iterator it(
            target, fs::directory_options::skip_permission_denied, ec);
        fs::directory_iterator end;
        if (ec) {
            return binding_error(L, "list: cannot read directory: " + target);
        }
        std::vector<fs::directory_entry> entries;
        for (; it != end; it.increment(ec)) {
            if (ec) {
                break;
            }
            entries.push_back(*it);
        }
        std::sort(entries.begin(), entries.end(),
            [](const fs::directory_entry& a, const fs::directory_entry& b) {
                return a.path().filename() < b.path().filename();
            });
        for (const auto& entry : entries) {
            std::error_code sec;
            const std::string name = entry.path().filename().string();
            if (!show_hidden && !name.empty() && name.front() == '.') {
                continue;
            }
            if (*count >= MAX_LIST_ENTRIES) {
                return 0;
            }
            ++*count;
            push_list_entry(L, entry, root, sec);
            lua_rawseti(L, -2, static_cast<lua_Integer>(*count));
            if (depth > 1 && entry.is_directory(sec)) {
                if (const int failed = list_directory(L, root,
                        entry.path().string(), depth - 1, show_hidden, count)) {
                    return failed;
                }
            }
        }
        return 0;
    }

    int binding_list(lua_State* L)
    {
        const std::string path = opt_string(L, 1).value_or(".");
        const int depth = static_cast<int>(opt_integer(L, 2).value_or(1));
        const bool show_hidden = opt_boolean(L, 3).value_or(false);
        if (depth < 1 || depth > MAX_LIST_DEPTH) {
            return binding_error(L,
                "list: depth must be between 1 and "
                    + std::to_string(MAX_LIST_DEPTH));
        }

        const ListDirectoryRequest request { path, depth, show_hidden };
        std::string target;
        if (const int denied = authorize_target(L, request, path, target)) {
            return denied;
        }

        std::error_code ec;
        if (!fs::is_directory(fs::path(target), ec)) {
            return binding_error(L,
                "list: no such directory: " + path + " (looked for " + target
                    + ")");
        }

        lua_newtable(L);
        int count = 0;
        if (const int failed = list_directory(
                L, fs::path(target), target, depth, show_hidden, &count)) {
            return failed;
        }
        return 1;
    }

    enum class GrepFileResult { COMPLETE, BINARY, UNREADABLE, TIMED_OUT };

    struct GrepState {
        lua_State* lua;
        const std::regex& expression;
        std::chrono::steady_clock::time_point deadline;
        int rows       = 0;
        bool truncated = false;
    };

    bool grep_timed_out(const GrepState& state)
    {
        return std::chrono::steady_clock::now() >= state.deadline;
    }

    void push_grep_row(GrepState& state, const fs::path& file,
        std::size_t line_number, const std::string& text)
    {
        ++state.rows;
        lua_newtable(state.lua);
        const std::string name = utf8_from_path(file);
        lua_pushlstring(state.lua, name.data(), name.size());
        lua_setfield(state.lua, -2, "file");
        lua_pushinteger(state.lua, static_cast<lua_Integer>(line_number));
        lua_setfield(state.lua, -2, "line");
        lua_pushlstring(state.lua, text.data(), text.size());
        lua_setfield(state.lua, -2, "text");
        lua_rawseti(state.lua, -2, static_cast<lua_Integer>(state.rows));
    }

    GrepFileResult grep_file(GrepState& state, const fs::path& file)
    {
        std::ifstream input(file, std::ios::binary);
        if (!input) {
            return GrepFileResult::UNREADABLE;
        }

        char buffer[8192];
        while (input) {
            if (grep_timed_out(state)) {
                return GrepFileResult::TIMED_OUT;
            }
            input.read(buffer, sizeof(buffer));
            if (std::find(buffer, buffer + input.gcount(), '\0')
                != buffer + input.gcount()) {
                return GrepFileResult::BINARY;
            }
        }
        if (!input.eof()) {
            return GrepFileResult::UNREADABLE;
        }

        input.clear();
        input.seekg(0);
        if (!input) {
            return GrepFileResult::UNREADABLE;
        }
        std::string line;
        std::size_t line_number = 0;
        while (std::getline(input, line)) {
            if (grep_timed_out(state)) {
                return GrepFileResult::TIMED_OUT;
            }
            ++line_number;
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (!std::regex_search(line, state.expression)) {
                continue;
            }
            if (state.rows == MAX_GREP_ROWS) {
                state.truncated = true;
                return GrepFileResult::COMPLETE;
            }
            push_grep_row(state, file, line_number, line);
        }
        return input.eof() ? GrepFileResult::COMPLETE
                           : GrepFileResult::UNREADABLE;
    }

    int grep_run(
        lua_State* L, const std::regex& expression, const fs::path& target)
    {
        constexpr auto timeout              = std::chrono::seconds { 10 };
        const std::string timed_out_message = "grep: search timed out after "
            + std::to_string(timeout.count()) + " seconds";
        GrepState state { L, expression,
            std::chrono::steady_clock::now() + timeout };
        lua_newtable(L);

        std::error_code ec;
        const fs::file_status target_status = fs::status(target, ec);
        if (ec) {
            return binding_error(
                L, "grep: cannot inspect target: " + utf8_from_path(target));
        }
        if (fs::is_regular_file(target_status)) {
            const GrepFileResult result = grep_file(state, target);
            if (result == GrepFileResult::TIMED_OUT) {
                return binding_error(L, timed_out_message);
            }
            if (result == GrepFileResult::UNREADABLE) {
                return binding_error(
                    L, "grep: cannot read file: " + utf8_from_path(target));
            }
        } else if (fs::is_directory(target_status)) {
            fs::recursive_directory_iterator iterator(
                target, fs::directory_options::skip_permission_denied, ec);
            const fs::recursive_directory_iterator end;
            if (ec) {
                return binding_error(L,
                    "grep: cannot read directory: " + utf8_from_path(target));
            }
            while (iterator != end && !state.truncated) {
                if (grep_timed_out(state)) {
                    return binding_error(L, timed_out_message);
                }
                const fs::directory_entry entry = *iterator;
                std::error_code status_error;
                const fs::file_status status
                    = entry.symlink_status(status_error);
                if (!status_error && fs::is_regular_file(status)) {
                    const GrepFileResult result
                        = grep_file(state, entry.path());
                    if (result == GrepFileResult::TIMED_OUT) {
                        return binding_error(L, timed_out_message);
                    }
                }
                iterator.increment(ec);
                if (ec) {
                    ec.clear();
                }
            }
        } else {
            return binding_error(L,
                "grep: target is not a file or directory: "
                    + utf8_from_path(target));
        }

        if (state.truncated) {
            lua_newtable(L);
            lua_pushboolean(L, 0);
            lua_setfield(L, -2, "file");
            lua_pushinteger(L, 0);
            lua_setfield(L, -2, "line");
            lua_pushliteral(L, "[truncated]");
            lua_setfield(L, -2, "text");
            lua_rawseti(L, -2, static_cast<lua_Integer>(state.rows + 1));
        }
        return 1;
    }

    int binding_grep(lua_State* L)
    {
        const std::string path    = opt_string(L, 1).value_or(".");
        const std::string pattern = luaL_checkstring(L, 2);
        if (pattern.empty()) {
            return binding_error(L, "grep: pattern must be a non-empty string");
        }

        std::regex expression;
        expression.imbue(std::locale::classic());
        try {
            expression.assign(pattern, std::regex_constants::extended);
        } catch (const std::regex_error& error) {
            return binding_error(L,
                "grep: invalid POSIX extended regular expression: "
                    + std::string(error.what()));
        }

        const FindFilesRequest request { path_from_utf8(path), pattern };
        std::string target;
        if (const int denied = authorize_target(L, request, path, target)) {
            return denied;
        }

        return grep_run(L, expression, fs::path(target));
    }

    // The run's record for `target`, or nullptr when the file has not been
    // mutated yet this run.
    FileMutation* find_mutation(LuaRunContext* run, const std::string& target)
    {
        for (FileMutation& m : run->mutations) {
            if (m.path == target) {
                return &m;
            }
        }
        return nullptr;
    }

    // Reads the file at `target` (the run's cached latest once touched),
    // applies `transform`, persists, and records the net mutation for the
    // run's final diff. `whole_file` (imza.fs.write) tolerates a missing
    // target: the original is then empty, so a fresh file diffs from blank.
    bool apply_file_mutation(lua_State* L, const std::string& target,
        const std::function<std::optional<std::string>(
            const std::string&, std::string&)>& transform,
        std::string& err, bool whole_file = false)
    {
        LuaRunContext* run     = run_of(L);
        FileMutation* mutation = find_mutation(run, target);
        std::string original;
        std::string content;
        if (mutation != nullptr) {
            original = mutation->original;
            content  = mutation->latest;
        } else {
            // load_text distinguishes a missing file (whole_file: a fresh
            // write from empty) from unreadable content (always an error).
            if (!load_text(target, content, err)) {
                if (!whole_file || !err.starts_with("no such file")) {
                    return false;
                }
                err.clear();
                content.clear();
            }
            original = content;
        }
        std::optional<std::string> next = transform(content, err);
        if (!next) {
            return false;
        }
        if (!save_text(target, *next, err)) {
            return false;
        }
        if (mutation == nullptr) {
            run->mutations.push_back({ target, original, *next });
        } else {
            mutation->latest = *next;
        }
        return true;
    }

    int binding_file_insert(lua_State* L)
    {
        const std::string path = luaL_checkstring(L, 1);
        const std::string text = luaL_checkstring(L, 2);
        lua_Integer line       = 0;
        if (const auto given = opt_integer(L, 3)) {
            line = *given;
            if (line < 1) {
                return binding_error(L, "fs.insert: line must be 1-based");
            }
        }

        const InsertFileRequest request { path, text,
            line > 0
                ? std::optional<std::size_t>(static_cast<std::size_t>(line))
                : std::nullopt };
        std::string target;
        if (const int denied = authorize_target(L, request, path, target)) {
            return denied;
        }

        std::string err;
        const std::size_t at = static_cast<std::size_t>(line);
        if (!apply_file_mutation(
                L, target,
                [&](const std::string& content, std::string& error) {
                    return insert_text(content, text, at, error);
                },
                err)) {
            return binding_error(L,
                "fs.insert: " + target + ": " + err
                    + (err.starts_with("no such file")
                            ? " (use fs.write to create it)"
                            : ""));
        }
        return 0;
    }

    int binding_file_edit(lua_State* L)
    {
        const std::string path  = luaL_checkstring(L, 1);
        const std::string old   = luaL_checkstring(L, 2);
        const std::string fresh = luaL_checkstring(L, 3);
        lua_Integer count       = 1;
        if (const auto given = opt_integer(L, 4)) {
            count = *given;
            if (count < 0) {
                return binding_error(L, "fs.edit: count must be 0 or more");
            }
        }
        if (old.empty()) {
            return binding_error(L, "fs.edit: old must be non-empty");
        }

        const EditFileRequest request { path, old, fresh,
            static_cast<std::size_t>(count) };
        std::string target;
        if (const int denied = authorize_target(L, request, path, target)) {
            return denied;
        }

        std::string err;
        if (!apply_file_mutation(
                L, target,
                [&](const std::string& content, std::string& error) {
                    return replace_text(content, old, fresh,
                        static_cast<std::size_t>(count), error);
                },
                err)) {
            return binding_error(L, "fs.edit: " + target + ": " + err);
        }
        return 0;
    }

    int binding_file_write(lua_State* L)
    {
        const std::string path = luaL_checkstring(L, 1);
        const std::string text = luaL_checkstring(L, 2);

        const WriteFileRequest request { path, text };
        std::string target;
        if (const int denied = authorize_target(L, request, path, target)) {
            return denied;
        }

        std::string err;
        if (!apply_file_mutation(
                L, target,
                [&](const std::string&, std::string&) {
                    return std::optional<std::string>(text);
                },
                err, true)) {
            return binding_error(L, "fs.write: " + target + ": " + err);
        }
        return 0;
    }

    // Per-node text cap: enough to identify a node without shipping
    // whole translation units back to the model.
    constexpr std::size_t MAX_TS_NODE_TEXT = 400;
    constexpr std::size_t MAX_TS_MATCHES   = 200;

    struct ParserDeleter {
        void operator()(TSParser* parser) const { ts_parser_delete(parser); }
    };
    struct TreeDeleter {
        void operator()(TSTree* tree) const { ts_tree_delete(tree); }
    };
    struct QueryDeleter {
        void operator()(TSQuery* query) const { ts_query_delete(query); }
    };
    struct QueryCursorDeleter {
        void operator()(TSQueryCursor* cursor) const
        {
            ts_query_cursor_delete(cursor);
        }
    };

    using ParserPtr      = std::unique_ptr<TSParser, ParserDeleter>;
    using TreePtr        = std::unique_ptr<TSTree, TreeDeleter>;
    using QueryPtr       = std::unique_ptr<TSQuery, QueryDeleter>;
    using QueryCursorPtr = std::unique_ptr<TSQueryCursor, QueryCursorDeleter>;

    const TSLanguage* language_for_path(std::string_view path)
    {
        const std::filesystem::path file(path);
        const std::string filename = to_lower(file.filename().string());
        for (const LanguageEntry& entry : LANGUAGE_ENTRIES) {
            if (std::ranges::find(entry.filenames, filename)
                != entry.filenames.end()) {
                return entry.load_language();
            }
        }
        std::string extension = to_lower(file.extension().string());
        if (!extension.empty() && extension.front() == '.') {
            extension.erase(0, 1);
        }
        for (const LanguageEntry& entry : LANGUAGE_ENTRIES) {
            if (std::ranges::find(entry.extensions, extension)
                != entry.extensions.end()) {
                return entry.load_language();
            }
        }
        return nullptr;
    }

    // One fresh parser per call: a script parses a handful of files, so
    // the UI's cached-parser machinery is not worth its locking here.
    ParserPtr make_parser(const TSLanguage* language)
    {
        ParserPtr parser(ts_parser_new());
        if (parser == nullptr
            || !ts_parser_set_language(parser.get(), language)) {
            parser.reset();
        }
        return parser;
    }

    struct PredicateStep {
        bool is_capture   = false;
        uint32_t value_id = 0;
    };

    std::string_view ts_node_text(std::string_view code, const TSNode& node)
    {
        return code.substr(ts_node_start_byte(node),
            ts_node_end_byte(node) - ts_node_start_byte(node));
    }

    struct Parsed {
        std::string code;
        TreePtr tree;
    };

    // Loads and parses `path`; the caller has already resolved the
    // grammar. Nullopt only after the binding error has been raised.
    std::optional<Parsed> parse_file(
        lua_State* L, const TSLanguage* language, const std::string& path)
    {
        if (language == nullptr) {
            binding_error(L, "ts_query: no grammar registered for: " + path);
            return std::nullopt;
        }
        std::string err;
        std::string code;
        if (!load_text(path, code, err)) {
            binding_error(L, "ts_query: " + err + " (looked for " + path + ")");
            return std::nullopt;
        }
        if (code.size() > std::numeric_limits<std::uint32_t>::max()) {
            binding_error(L, "ts_query: file too large: " + path);
            return std::nullopt;
        }
        const ParserPtr parser = make_parser(language);
        if (parser == nullptr) {
            binding_error(L, "ts_query: cannot create parser for: " + path);
            return std::nullopt;
        }
        TreePtr tree(ts_parser_parse_string(parser.get(), nullptr, code.data(),
            static_cast<uint32_t>(code.size())));
        if (tree == nullptr) {
            binding_error(L, "ts_query: parse failed: " + path);
            return std::nullopt;
        }
        return Parsed { std::move(code), std::move(tree) };
    }

    std::string query_error_message(
        TSQueryError error, std::string_view query, uint32_t offset)
    {
        const char* kind = "syntax error";
        switch (error) {
        case TSQueryErrorSyntax: break;
        case TSQueryErrorNodeType: kind = "unknown node type"; break;
        case TSQueryErrorField: kind = "unknown field"; break;
        case TSQueryErrorCapture: kind = "invalid capture"; break;
        case TSQueryErrorStructure: kind = "pattern structure error"; break;
        case TSQueryErrorLanguage: kind = "language error"; break;
        case TSQueryErrorNone: break;
        }
        // The offset lands at or after the offending token; show the
        // nearby tail of the query so the message identifies it.
        const std::size_t begin = offset > 40 ? offset - 40 : 0;
        std::string_view tail   = query.substr(begin, offset - begin);
        return "ts_query: " + std::string(kind) + " at byte "
            + std::to_string(offset) + " near \""
            + std::string(tail.substr(
                tail.find_first_not_of(" \\t\\n") == std::string::npos
                    ? 0
                    : tail.find_first_not_of(" \\t\\n")))
            + "\"";
    }

    // Null only after the binding error has been raised.
    QueryPtr compile_query(
        lua_State* L, const TSLanguage* language, const std::string& query)
    {
        uint32_t offset   = 0;
        TSQueryError type = TSQueryErrorNone;
        QueryPtr compiled(ts_query_new(language, query.data(),
            static_cast<uint32_t>(query.size()), &offset, &type));
        if (compiled == nullptr) {
            binding_error(L, query_error_message(type, query, offset));
        }
        return compiled;
    }

    // The text bound to a predicate argument: the capture's own text, or
    // the text of the capture named by the ".suffix" form (e.g.
    // @function.kind) as a field access on a child node.
    std::string predicate_capture_text(const TSQuery* query,
        const TSQueryMatch& match, std::string_view code,
        const std::string& argument)
    {
        const std::size_t dot = argument.find('.');
        const std::string name
            = dot == std::string::npos ? argument : argument.substr(0, dot);
        const uint32_t count = ts_query_capture_count(query);
        for (uint32_t id = 0; id < count; ++id) {
            uint32_t length = 0;
            const char* capture
                = ts_query_capture_name_for_id(query, id, &length);
            if (std::string_view(capture, length) != name) {
                continue;
            }
            for (uint16_t i = 0; i < match.capture_count; ++i) {
                if (match.captures[i].index != id) {
                    continue;
                }
                if (dot == std::string::npos) {
                    return std::string(
                        ts_node_text(code, match.captures[i].node));
                }
                const TSNode node       = match.captures[i].node;
                const std::string field = argument.substr(dot + 1);
                const uint32_t children = ts_node_child_count(node);
                for (uint32_t child = 0; child < children; ++child) {
                    const char* child_field
                        = ts_node_field_name_for_child(node, child);
                    if (child_field != nullptr && field == child_field) {
                        const TSNode found = ts_node_child(node, child);
                        if (!ts_node_is_null(found)) {
                            return std::string(ts_node_text(code, found));
                        }
                    }
                }
                return { };
            }
        }
        return { };
    }

    std::string predicate_string_value(const TSQuery* query, uint32_t value_id)
    {
        uint32_t length = 0;
        const char* value
            = ts_query_string_value_for_id(query, value_id, &length);
        return std::string(value, length);
    }

    // Evaluates the portable predicate set (#eq?, #not-eq?, #match?,
    // #not-match?, #any-of?, #not-any-of?) against capture text.
    // Unknown predicates are ignored rather than fatal.
    bool predicates_hold(
        const TSQuery* query, const TSQueryMatch& match, std::string_view code)
    {
        uint32_t step_count               = 0;
        const TSQueryPredicateStep* steps = ts_query_predicates_for_pattern(
            query, match.pattern_index, &step_count);
        std::vector<PredicateStep> predicate;
        const auto argument_text = [&](const PredicateStep& step) {
            if (!step.is_capture) {
                return predicate_string_value(query, step.value_id);
            }
            uint32_t length = 0;
            const char* name
                = ts_query_capture_name_for_id(query, step.value_id, &length);
            return predicate_capture_text(
                query, match, code, std::string(name, length));
        };
        const auto evaluate = [&]() {
            // The operator is always a string step; a leading capture
            // step is malformed, not something to filter on.
            if (predicate.size() < 2 || predicate[0].is_capture) {
                return true;
            }
            const std::string op
                = predicate_string_value(query, predicate[0].value_id);
            if (op != "#eq?" && op != "#not-eq?" && op != "#match?"
                && op != "#not-match?" && op != "#any-of?"
                && op != "#not-any-of?") {
                return true; // not ours to judge
            }
            if (predicate.size() < 3) {
                return true;
            }
            const std::string left = argument_text(predicate[1]);
            if (op == "#eq?" || op == "#not-eq?") {
                if (predicate.size() != 3) {
                    return true;
                }
                const bool equal = left == argument_text(predicate[2]);
                return op == "#eq?" ? equal : !equal;
            }
            if (op == "#any-of?" || op == "#not-any-of?") {
                bool any = false;
                for (std::size_t i = 2; i < predicate.size(); ++i) {
                    if (left == argument_text(predicate[i])) {
                        any = true;
                        break;
                    }
                }
                return op == "#any-of?" ? any : !any;
            }
            // #match? / #not-match?: ECMAScript regex over capture text.
            if (predicate.size() != 3 || predicate[2].is_capture) {
                return true;
            }
            try {
                const std::regex expression(argument_text(predicate[2]));
                const bool hit = std::regex_search(left, expression);
                return op == "#match?" ? hit : !hit;
            } catch (const std::regex_error&) {
                return true; // unusable pattern: do not silently drop rows
            }
        };
        for (uint32_t i = 0; i < step_count; ++i) {
            const TSQueryPredicateStep& step = steps[i];
            if (step.type == TSQueryPredicateStepTypeDone) {
                if (!evaluate()) {
                    return false;
                }
                predicate.clear();
                continue;
            }
            predicate.push_back({ step.type == TSQueryPredicateStepTypeCapture,
                step.value_id });
        }
        return predicate.empty() || evaluate();
    }

    // One result row: { file, captures = { name = TsCapture } }.
    void push_match_row(lua_State* L, const std::string& file,
        const TSQuery* query, const TSQueryMatch& match, std::string_view code)
    {
        lua_newtable(L);
        lua_pushlstring(L, file.data(), file.size());
        lua_setfield(L, -2, "file");
        lua_newtable(L);
        for (uint16_t i = 0; i < match.capture_count; ++i) {
            const TSNode node = match.captures[i].node;
            uint32_t length   = 0;
            const char* raw   = ts_query_capture_name_for_id(
                query, match.captures[i].index, &length);
            lua_pushlstring(L, raw, length);
            lua_newtable(L);
            const std::string_view kind(ts_node_type(node));
            lua_pushlstring(L, kind.data(), kind.size());
            lua_setfield(L, -2, "kind");
            const std::string body
                = truncate_marked(ts_node_text(code, node), MAX_TS_NODE_TEXT);
            lua_pushlstring(L, body.data(), body.size());
            lua_setfield(L, -2, "text");
            lua_pushinteger(L, ts_node_start_point(node).row + 1);
            lua_setfield(L, -2, "start_line");
            lua_pushinteger(L, ts_node_end_point(node).row + 1);
            lua_setfield(L, -2, "end_line");
            lua_settable(L, -3);
        }
        lua_setfield(L, -2, "captures");
    }

    struct TsQueryState {
        lua_State* lua;
        std::size_t rows = 0;
        bool truncated   = false;
    };

    // Appends one row per match; false once the match cap is hit.
    bool ts_query_run_file(TsQueryState& state, const TSQuery* query,
        const Parsed& parsed, const std::string& file)
    {
        lua_State* L = state.lua;
        QueryCursorPtr cursor(ts_query_cursor_new());
        if (cursor == nullptr) {
            binding_error(L, "ts_query: cannot create query cursor");
        }
        ts_query_cursor_exec(
            cursor.get(), query, ts_tree_root_node(parsed.tree.get()));

        TSQueryMatch match;
        while (ts_query_cursor_next_match(cursor.get(), &match)) {
            if (!predicates_hold(query, match, parsed.code)) {
                continue;
            }
            if (state.rows == MAX_TS_MATCHES) {
                state.truncated = true;
                return false;
            }
            ++state.rows;
            push_match_row(L, file, query, match, parsed.code);
            lua_rawseti(L, -2, static_cast<lua_Integer>(state.rows));
        }
        return true;
    }

    // Per-language compiled queries for a directory walk: grammars
    // interleave, and a query is bound to one language.
    struct QueryCache {
        std::vector<std::pair<const TSLanguage*, QueryPtr>> entries;

        const TSQuery* get_or_compile(
            lua_State* L, const TSLanguage* language, const std::string& query)
        {
            for (const auto& [cached_language, cached] : entries) {
                if (cached_language == language) {
                    return cached.get();
                }
            }
            QueryPtr compiled = compile_query(L, language, query);
            entries.emplace_back(language, std::move(compiled));
            return entries.back().second.get();
        }
    };

    int binding_ts_query(lua_State* L)
    {
        const std::string path  = opt_string(L, 1).value_or(".");
        const std::string query = luaL_checkstring(L, 2);
        if (query.empty()) {
            return binding_error(
                L, "ts_query: query must be a non-empty string");
        }

        const FindFilesRequest request { path_from_utf8(path), query };
        std::string target;
        if (const int denied = authorize_target(L, request, path, target)) {
            return denied;
        }

        std::error_code ec;
        const fs::file_status status = fs::status(fs::path(target), ec);
        if (ec) {
            return binding_error(
                L, "ts_query: cannot inspect target: " + target);
        }

        lua_newtable(L);
        TsQueryState state { L };
        if (fs::is_regular_file(status)) {
            const TSLanguage* language = language_for_path(target);
            if (language == nullptr) {
                return binding_error(
                    L, "ts_query: no grammar registered for: " + path);
            }
            // Compile first: an invalid pattern is a script bug and
            // should not depend on the target being parseable.
            const QueryPtr compiled = compile_query(L, language, query);
            if (compiled == nullptr) {
                return 2;
            }
            const auto parsed = parse_file(L, language, target);
            if (!parsed) {
                return 2;
            }
            ts_query_run_file(state, compiled.get(), *parsed, target);
        } else if (fs::is_directory(status)) {
            const LuaRunContext& run = *run_of(L);
            QueryCache cache;
            for (auto it = fs::recursive_directory_iterator(
                     target, fs::directory_options::skip_permission_denied, ec);
                it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) {
                    ec.clear();
                    continue;
                }
                if (state.rows == MAX_TS_MATCHES) {
                    state.truncated = true;
                    break;
                }
                if (std::chrono::steady_clock::now() > run.deadline) {
                    return binding_error(L, "ts_query: search timed out");
                }
                std::error_code file_ec;
                if (!it->is_regular_file(file_ec) || file_ec) {
                    continue;
                }
                const std::string file     = utf8_from_path(it->path());
                const TSLanguage* language = language_for_path(file);
                if (language == nullptr) {
                    continue; // unrecognized files are skipped, not errors
                }
                const auto parsed = parse_file(L, language, file);
                if (!parsed) {
                    return 2;
                }
                const TSQuery* compiled
                    = cache.get_or_compile(L, language, query);
                if (compiled == nullptr) {
                    return 2;
                }
                if (!ts_query_run_file(state, compiled, *parsed, file)) {
                    break;
                }
            }
        } else {
            return binding_error(
                L, "ts_query: target is not a file or directory: " + path);
        }

        if (state.truncated) {
            lua_newtable(L);
            lua_pushboolean(L, 0);
            lua_setfield(L, -2, "file");
            lua_pushliteral(L, "[truncated]");
            lua_setfield(L, -2, "text");
            lua_rawseti(L, -2, static_cast<lua_Integer>(state.rows + 1));
        }
        return 1;
    }

    constexpr LuaMethod BINDINGS[] = {
        {
            "read",
            binding_read,
            R"desc((path: string, first_line?: integer=1 last_line?: integer=nil) returns string, throws
Read the file at `path` returning its content.
first_line..last_line inclusive omit last_line to read to the end.
Throws on no such file, first_line past the end, last_line < first_line, or a
binary file. Over 64 KB is cut and marked "[truncated]".)desc",
        },
        {
            "list",
            binding_list,
            R"desc((path?: string=".", depth?: integer=1, show_hidden?: bool=false) returns FileEntry[], throws
List files and directories in `path`, returning their paths and sizes.
Paths are relative to the requested directory. Filename-sorted listing;
depth (1..5) descends into subdirectories and their entries come back flat.
`size` is absent for directories and "-" when unreadable.
Capped at 2000 entries. Throws when the directory is missing or
unreadable.)desc",
        },
        {
            "grep",
            binding_grep,
            R"desc((path: string, pattern: string) returns GrepHit[], throws
Run a POSIX extended regex (not a Lua pattern) over a file or directory tree,
one hit per matching line.
Capped at 500 hits, followed by a hit whose text is "[truncated]".)desc",
        },
        {
            "insert",
            binding_file_insert,
            R"desc((path: string, text: string, line?: integer=nil) throws
Inserts text before the 1-based line, pushing it down; omit line to
append at the end. Throws when the line is past the end of the file or
the file is missing (use fs.write to create it).)desc",
        },
        {
            "edit",
            binding_file_edit,
            R"desc((path: string, old: string, new: string, count?: integer=1) throws
Replaces the first count occurrences of old with new; count=0 replaces all.
Old is an exact literal match, so include enough surrounding text to be unique.
Throws when old is empty or not found; wrap in pcall when not-found is
expected.)desc",
        },
        {
            "write",
            binding_file_write,
            R"desc((path: string, text: string) throws
Replaces the file's entire content, creating it (and missing parent
directories) if absent.
Prefer insert/edit for targeted changes; this discards everything else.)desc",
        },
        {
            "ts_query",
            binding_ts_query,
            R"desc((path: string, query: string) returns TsQueryMatch[], throws
Run a tree-sitter query over a file, or over every grammar-recognized
file under a directory. `query` is the standard tree-sitter S-expression
pattern language: node types, fields, captures, quantifiers, alternations,
and the portable predicates (#eq?, #not-eq?, #match?, #not-match?,
#any-of?, #not-any-of?); captures are keyed by name.
A single-file target with no registered grammar throws; a directory skips
files without one. Directory walks also enforce the run's time budget.
Capped at 200 matches, followed by a marker row whose text is
"[truncated]".)desc",
        },
    };

} // namespace

const LuaModule& fs_module()
{
    static constexpr std::string_view types[] = {
        "FileEntry = { path: string, type: \\\"file\\\" | \\\"dir\\\", size?: "
        "string }",
        "GrepHit = { file: string, line: integer, text: string }",
        "TsCapture = { kind: string, text: string, start_line: integer, "
        "end_line: integer }",
        "TsQueryMatch = { file: string, captures: { [name]: TsCapture } }",
    };
    static constexpr LuaModule MODULE { true, "fs",
        "Filesystem inspection and mutation.", types, BINDINGS };
    return MODULE;
}

} // namespace imza
