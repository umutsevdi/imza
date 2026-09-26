#include "tools/bindings.h"

#include "common/util.h"
#include "tools/file_ops.h"

#include <tree_sitter/api.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace imza {

#include "language_registry.inc"

namespace {

    constexpr std::size_t MAX_SYMBOLS    = 50;
    constexpr std::size_t MAX_NODES      = 500;
    constexpr std::size_t MAX_QUERY_ROWS = 500;
    // Per-node text cap: enough to identify a declaration without
    // shipping whole translation units back to the model.
    constexpr std::size_t MAX_NODE_TEXT = 400;

    struct ParserDeleter {
        void operator()(TSParser* parser) const { ts_parser_delete(parser); }
    };
    struct TreeDeleter {
        void operator()(TSTree* tree) const { ts_tree_delete(tree); }
    };
    struct QueryDeleter {
        void operator()(TSQuery* query) const { ts_query_delete(query); }
    };
    struct CursorDeleter {
        void operator()(TSQueryCursor* cursor) const
        {
            ts_query_cursor_delete(cursor);
        }
    };

    using ParserPtr = std::unique_ptr<TSParser, ParserDeleter>;
    using TreePtr   = std::unique_ptr<TSTree, TreeDeleter>;
    using QueryPtr  = std::unique_ptr<TSQuery, QueryDeleter>;
    using CursorPtr = std::unique_ptr<TSQueryCursor, CursorDeleter>;

    // Grammar entry point for the file's filename or lowercased
    // extension; nullptr when no registered grammar claims the path.
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

    // One fresh parser per call: a script runs a handful of parses, so
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

    // Shallowest identifier in `node`'s subtree: the `name` field when
    // the grammar defines one, else the first child of grammar type
    // `identifier`. Null node when it has neither.
    TSNode node_name_inner(const TSNode& node, std::string_view)
    {
        std::vector<TSNode> stack { node };
        while (!stack.empty()) {
            const TSNode current = stack.back();
            stack.pop_back();
            if (std::string_view(ts_node_type(current)) == "identifier") {
                return current;
            }
            const uint32_t count = ts_node_named_child_count(current);
            for (uint32_t i = count; i > 0; --i) {
                stack.push_back(ts_node_named_child(current, i - 1));
            }
        }
        return TSNode { };
    }

    std::string node_name(const TSNode& node, std::string_view code)
    {
        TSNode name_node { };
        // Field indices are 1-based over ALL children (named and
        // anonymous), so iterate the full child list for the field scan.
        const uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            const char* field = ts_node_field_name_for_child(node, i);
            if (field == nullptr) {
                continue;
            }
            const TSNode child = ts_node_child(node, i);
            if (std::strcmp(field, "name") == 0) {
                name_node = child;
                break;
            }
        }
        if (ts_node_is_null(name_node)) {
            const uint32_t named = ts_node_named_child_count(node);
            for (uint32_t i = 0; i < named; ++i) {
                const TSNode child = ts_node_named_child(node, i);
                if (std::string_view(ts_node_type(child)) == "identifier") {
                    name_node = child;
                    break;
                }
            }
        }
        // C-family grammars nest the identifier under a `declarator`
        // field (function definitions and declarations); follow it one
        // level so `int beta(...)` reports `beta`.
        if (ts_node_is_null(name_node)) {
            for (uint32_t i = 0; i < count; ++i) {
                const char* field = ts_node_field_name_for_child(node, i);
                if (field != nullptr && std::strcmp(field, "declarator") == 0) {
                    name_node = ts_node_child(node, i);
                    break;
                }
            }
            if (!ts_node_is_null(name_node)) {
                name_node = node_name_inner(name_node, code);
            }
        }
        if (ts_node_is_null(name_node)) {
            return { };
        }
        const uint32_t begin = ts_node_start_byte(name_node);
        const uint32_t end   = ts_node_end_byte(name_node);
        return std::string(code.substr(begin, end - begin));
    }

    bool glob_match(std::string_view pattern, std::string_view text)
    {
        if (pattern.empty()) {
            return text.empty();
        }
        if (pattern.front() == '*') {
            for (std::size_t i = 0; i <= text.size(); ++i) {
                if (glob_match(pattern.substr(1), text.substr(i))) {
                    return true;
                }
            }
            return false;
        }
        return !text.empty() && pattern.front() == text.front()
            && glob_match(pattern.substr(1), text.substr(1));
    }

    void push_symbol(lua_State* L, const TSNode& node, std::string_view code)
    {
        const uint32_t begin = ts_node_start_byte(node);
        const uint32_t end   = ts_node_end_byte(node);
        lua_newtable(L);
        lua_pushliteral(L, "kind");
        lua_pushlstring(L, ts_node_type(node), std::strlen(ts_node_type(node)));
        lua_settable(L, -3);
        const std::string name = node_name(node, code);
        lua_pushliteral(L, "name");
        lua_pushlstring(L, name.data(), name.size());
        lua_settable(L, -3);
        lua_pushliteral(L, "start_line");
        lua_pushinteger(L, ts_node_start_point(node).row + 1);
        lua_settable(L, -3);
        lua_pushliteral(L, "end_line");
        lua_pushinteger(L, ts_node_end_point(node).row + 1);
        lua_settable(L, -3);
        std::string body(code.substr(
            begin, std::min<std::size_t>(end - begin, MAX_NODE_TEXT)));
        if (end - begin > MAX_NODE_TEXT) {
            body += "\n[truncated]";
        }
        lua_pushliteral(L, "text");
        lua_pushlstring(L, body.data(), body.size());
        lua_settable(L, -3);
    }

    // Declaration-ish grammar kinds, matched by substring so every
    // registered grammar benefits without per-language tables.
    bool is_declaration_kind(std::string_view kind)
    {
        // "field_declaration" rows are member fields, not symbols; the
        // declaration family still covers real declarations elsewhere.
        return kind.find("definition") != std::string_view::npos
            || (kind.find("declaration") != std::string_view::npos
                && !kind.starts_with("field"))
            || kind.starts_with("function") || kind.starts_with("class")
            || kind.starts_with("struct") || kind.starts_with("interface")
            || kind.starts_with("enum") || kind.starts_with("method");
    }

    // Gate, load, and parse `path` at `index`. On failure returns null
    // with the binding-error convention already pushed (callers return 2).
    struct Parsed {
        std::string target;
        std::string code;
        TreePtr tree;
    };

    std::optional<Parsed> parse_file(lua_State* L, int index)
    {
        const std::string path = luaL_checkstring(L, index);
        const GateOutcome gate = authorize_filesystem(
            L, ReadFileRequest { path, 1, std::nullopt });
        if (!gate) {
            binding_error(L, gate_denied(L, gate.denial, path));
            return std::nullopt;
        }
        const std::string target = filesystem_target(*gate.filesystem).string();

        const TSLanguage* language = language_for_path(target);
        if (language == nullptr) {
            binding_error(L, "ts: no grammar registered for: " + path);
            return std::nullopt;
        }
        std::string err;
        std::string code;
        if (!load_text(target, code, err)) {
            binding_error(L, "ts: " + err + " (looked for " + target + ")");
            return std::nullopt;
        }
        if (code.size() > std::numeric_limits<std::uint32_t>::max()) {
            binding_error(L, "ts: file too large: " + path);
            return std::nullopt;
        }
        const ParserPtr parser = make_parser(language);
        if (parser == nullptr) {
            binding_error(L, "ts: cannot create parser for: " + path);
            return std::nullopt;
        }
        TreePtr tree(ts_parser_parse_string(parser.get(), nullptr, code.data(),
            static_cast<uint32_t>(code.size())));
        if (tree == nullptr) {
            binding_error(L, "ts: parse failed: " + path);
            return std::nullopt;
        }
        return Parsed { target, std::move(code), std::move(tree) };
    }

    int binding_ts_query(lua_State* L)
    {
        const auto parsed = parse_file(L, 1);
        if (!parsed) {
            return 2;
        }
        std::size_t query_size = 0;
        const char* query_raw  = luaL_checklstring(L, 2, &query_size);

        const TSLanguage* language = language_for_path(parsed->target);
        uint32_t error_offset      = 0;
        TSQueryError error_type    = TSQueryErrorNone;
        QueryPtr query(ts_query_new(language, query_raw,
            static_cast<uint32_t>(query_size), &error_offset, &error_type));
        if (query == nullptr) {
            return binding_error(L,
                "query: invalid query at byte " + std::to_string(error_offset));
        }
        CursorPtr cursor(ts_query_cursor_new());
        if (cursor == nullptr) {
            return binding_error(L, "query: cannot create cursor");
        }
        ts_query_cursor_exec(
            cursor.get(), query.get(), ts_tree_root_node(parsed->tree.get()));

        lua_newtable(L);
        int row = 0;
        TSQueryMatch match { };
        while (ts_query_cursor_next_match(cursor.get(), &match)) {
            if (static_cast<std::size_t>(row) >= MAX_QUERY_ROWS) {
                break;
            }
            for (uint16_t i = 0; i < match.capture_count
                && static_cast<std::size_t>(row) < MAX_QUERY_ROWS;
                ++i) {
                const TSQueryCapture& capture = match.captures[i];
                uint32_t name_size            = 0;
                const char* name              = ts_query_capture_name_for_id(
                    query.get(), capture.index, &name_size);
                const uint32_t begin = ts_node_start_byte(capture.node);
                const uint32_t end   = ts_node_end_byte(capture.node);
                lua_newtable(L);
                lua_pushlstring(L, name, name_size);
                lua_setfield(L, -2, "capture");
                lua_pushinteger(L, ts_node_start_point(capture.node).row + 1);
                lua_setfield(L, -2, "line");
                lua_pushlstring(L, parsed->code.data() + begin,
                    std::min<std::size_t>(end - begin, MAX_NODE_TEXT));
                lua_setfield(L, -2, "text");
                lua_rawseti(L, -2, ++row);
            }
        }
        return 1;
    }

    int binding_ts_index(lua_State* L)
    {
        const auto parsed = parse_file(L, 1);
        if (!parsed) {
            return 2;
        }
        lua_newtable(L);
        int row = 0;
        std::vector<TSNode> stack { ts_tree_root_node(parsed->tree.get()) };
        while (!stack.empty() && static_cast<std::size_t>(row) < MAX_SYMBOLS) {
            const TSNode node = stack.back();
            stack.pop_back();
            // Children are visited whether or not the parent matches:
            // grammars nest declarations inside translation units, classes,
            // and namespaces.
            const std::string_view kind = ts_node_type(node);
            if (is_declaration_kind(kind)) {
                push_symbol(L, node, parsed->code);
                lua_rawseti(L, -2, ++row);
            }
            const uint32_t count = ts_node_named_child_count(node);
            for (uint32_t i = count; i > 0; --i) {
                stack.push_back(ts_node_named_child(node, i - 1));
            }
        }
        return 1;
    }

    int binding_ts_nodes(lua_State* L)
    {
        std::size_t type_size = 0;
        const char* type_raw  = luaL_checklstring(L, 2, &type_size);
        const std::string_view type(type_raw, type_size);
        const bool exact = type.find('*') == std::string_view::npos;

        const auto parsed = parse_file(L, 1);
        if (!parsed) {
            return 2;
        }
        const TSLanguage* language = language_for_path(parsed->target);
        if (exact
            && ts_language_symbol_for_name(language, type.data(),
                   static_cast<uint32_t>(type.size()), true)
                == 0) {
            return binding_error(
                L, "nodes: unknown node type: " + std::string(type));
        }

        lua_newtable(L);
        int row = 0;
        std::vector<TSNode> stack { ts_tree_root_node(parsed->tree.get()) };
        while (!stack.empty() && static_cast<std::size_t>(row) < MAX_NODES) {
            const TSNode node = stack.back();
            stack.pop_back();
            const std::string_view kind = ts_node_type(node);
            if (exact ? kind == type : glob_match(type, kind)) {
                push_symbol(L, node, parsed->code);
                lua_rawseti(L, -2, ++row);
            }
            const uint32_t count = ts_node_named_child_count(node);
            for (uint32_t i = count; i > 0; --i) {
                stack.push_back(ts_node_named_child(node, i - 1));
            }
        }
        return 1;
    }

    // Full source line of the byte at `begin`, for context rows.
    std::string_view line_at(std::string_view code, uint32_t begin)
    {
        const std::size_t line_begin
            = code.rfind('\n', begin) == std::string::npos
            ? 0
            : code.rfind('\n', begin) + 1;
        const std::size_t line_end = code.find('\n', begin);
        const std::size_t stop
            = line_end == std::string::npos ? code.size() : line_end;
        return code.substr(line_begin, stop - line_begin);
    }

    int binding_ts_symbols(lua_State* L)
    {
        std::size_t symbol_size = 0;
        const char* symbol_raw  = luaL_checklstring(L, 2, &symbol_size);
        const std::string_view symbol(symbol_raw, symbol_size);

        const auto parsed = parse_file(L, 1);
        if (!parsed) {
            return 2;
        }

        lua_newtable(L);
        int row = 0;
        std::vector<TSNode> stack { ts_tree_root_node(parsed->tree.get()) };
        while (!stack.empty()) {
            const TSNode node = stack.back();
            stack.pop_back();
            const uint32_t count = ts_node_named_child_count(node);
            for (uint32_t i = count; i > 0; --i) {
                stack.push_back(ts_node_named_child(node, i - 1));
            }
            if (std::string_view(ts_node_type(node)) != "identifier") {
                continue;
            }
            const uint32_t begin = ts_node_start_byte(node);
            const uint32_t end   = ts_node_end_byte(node);
            if (parsed->code.substr(begin, end - begin) != symbol) {
                continue;
            }
            lua_newtable(L);
            lua_pushlstring(L, parsed->target.data(), parsed->target.size());
            lua_setfield(L, -2, "file");
            lua_pushinteger(L, ts_node_start_point(node).row + 1);
            lua_setfield(L, -2, "line");
            const std::string_view line = line_at(parsed->code, begin);
            lua_pushlstring(L, line.data(), line.size());
            lua_setfield(L, -2, "text");
            lua_rawseti(L, -2, ++row);
        }
        return 1;
    }

    // Identifier nodes whose parent is a call node: call sites of the
    // given name. Textual, not a semantic resolution -- no macros,
    // virtual dispatch, or cross-file knowledge.
    int binding_ts_references(lua_State* L)
    {
        std::size_t symbol_size = 0;
        const char* symbol_raw  = luaL_checklstring(L, 2, &symbol_size);
        const std::string_view symbol(symbol_raw, symbol_size);

        const auto parsed = parse_file(L, 1);
        if (!parsed) {
            return 2;
        }

        lua_newtable(L);
        int row = 0;
        std::vector<TSNode> stack { ts_tree_root_node(parsed->tree.get()) };
        while (!stack.empty() && static_cast<std::size_t>(row) < MAX_NODES) {
            const TSNode node = stack.back();
            stack.pop_back();
            const uint32_t count = ts_node_named_child_count(node);
            for (uint32_t i = count; i > 0; --i) {
                stack.push_back(ts_node_named_child(node, i - 1));
            }
            if (std::string_view(ts_node_type(node)) != "identifier") {
                continue;
            }
            const uint32_t begin = ts_node_start_byte(node);
            const uint32_t end   = ts_node_end_byte(node);
            if (parsed->code.substr(begin, end - begin) != symbol) {
                continue;
            }
            const std::string_view parent = ts_node_type(ts_node_parent(node));
            if (parent.find("call") == std::string_view::npos) {
                continue;
            }
            lua_newtable(L);
            lua_pushinteger(L, ts_node_start_point(node).row + 1);
            lua_setfield(L, -2, "line");
            lua_pushlstring(L, parent.data(), parent.size());
            lua_setfield(L, -2, "kind");
            const std::string_view line = line_at(parsed->code, begin);
            lua_pushlstring(L, line.data(), line.size());
            lua_setfield(L, -2, "text");
            lua_rawseti(L, -2, ++row);
        }
        return 1;
    }

    constexpr LuaMethod BINDINGS[] = {
        {
            "_lib.ts_query",
            binding_ts_query,
            R"desc((path: string, query: string) => { capture, line, text }[]
#private: Execute an arbitrary tree-sitter query (S-expression pattern with
@captures) over the file, one row per capture.
Fails on an invalid query, naming the byte offset of the syntax error.
Capped at 500 rows.)desc",
            LuaCapability::NONE,
            "",
            true,
        },
        {
            "index",
            binding_ts_index,
            R"desc((path: string) => TsSymbol[]
List the named symbols in `path`: functions, methods, classes, structs,
interfaces, enums, and other declaration/definition nodes parsed by the
language grammar matched for the file's extension or name. Each entry
reports `kind` (the grammar node type), `name`, `start_line`..`end_line`
(1-based inclusive), and `text`.
Capped at 50 entries; use imza.tree.nodes with a narrower type for more.)desc",
            LuaCapability::NONE,
        },
        {
            "nodes",
            binding_ts_nodes,
            R"desc((path: string, type: string) => TsSymbol[]
List every node in `path` whose grammar type name matches `type`: an exact
grammar node type (e.g. "function_definition", "class_specifier") or a `*`
glob ("*call*"). Entries carry `kind`, `name`, `start_line`..`end_line`,
and `text`.
Fails on an unknown exact type, naming it.
Capped at 500 entries.)desc",
            LuaCapability::NONE,
        },
        {
            "symbols",
            binding_ts_symbols,
            R"desc((path: string, symbol: string) => { file, line, text }[]
List every occurrence of `symbol` (an identifier node) in `path`, one row
per use with the file path, 1-based `line`, and the full source line as
`text`. Grammar-typed, so comments and strings never match.)desc",
            LuaCapability::NONE,
        },
        {
            "references",
            binding_ts_references,
            R"desc((path: string, symbol: string) => { line, kind, text }[]
List the call sites of `symbol` in `path`: identifier nodes inside a call
node, one row per site with 1-based `line`, the call node's grammar `kind`,
and the full source line as `text`. Textual call-site matching, not a
semantic resolution -- macros, virtual dispatch, and other files are out of
scope. Capped at 500 entries.)desc",
            LuaCapability::NONE,
        },
    };

} // namespace

std::span<const LuaMethod> tree_lua_methods() { return BINDINGS; }

void register_tree(LuaState& state)
{
    static constexpr std::string_view types[] = {
        "TsSymbol = { kind: string, name: string, start_line: integer, "
        "end_line: integer, text: string }",
    };
    state.register_module({ false, "tree",
        R"desc(Syntax tree inspection: list the symbols a file defines, find where
an identifier appears, and enumerate call sites. Load for questions about
code structure and references.)desc",
        types, tree_lua_methods() });
}

} // namespace imza
