#include "tools/bindings.h"

#include "common/util.h"
#include "permissions/filesystem.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <regex>
#include <sstream>
#include <string>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace imza {
namespace {

    namespace fs = std::filesystem;

    constexpr int MAX_LIST_ENTRIES = 2000;
    constexpr int MAX_GREP_ROWS    = 500;
    constexpr int MAX_LIST_DEPTH   = 5;

    int binding_read(lua_State* L)
    {
        const std::string path  = luaL_checkstring(L, 1);
        const lua_Integer first = lua_gettop(L) >= 2 && !lua_isnil(L, 2)
            ? luaL_checkinteger(L, 2)
            : 1;
        const bool last_given   = lua_gettop(L) >= 3 && !lua_isnil(L, 3);
        const lua_Integer last  = last_given ? luaL_checkinteger(L, 3) : 0;
        if (first < 1 || (last_given && last < first)) {
            return binding_error(L,
                "read: line range must be 1-based and last_line >= first_line");
        }

        const ReadFileRequest request { path, static_cast<std::size_t>(first),
            last_given
                ? std::optional<std::size_t>(static_cast<std::size_t>(last))
                : std::nullopt };
        const GateOutcome gate = authorize_filesystem(L, request);
        if (!gate) {
            return binding_error(L, gate_denied(L, gate.denial, path));
        }
        const std::string target = filesystem_target(*gate.filesystem).string();

        std::error_code ec;
        if (!fs::is_regular_file(fs::path(target), ec)) {
            return binding_error(L,
                "read: no such file: " + path + " (looked for " + target + ")");
        }
        std::ifstream in(target, std::ios::binary);
        if (!in) {
            return binding_error(L, "read: cannot open: " + path);
        }

        lua_Integer number = 0;
        std::string content;
        std::string line;
        while (std::getline(in, line)) {
            ++number;
            if (number >= first && (!last_given || number <= last)) {
                if (line.find('\0') != std::string::npos) {
                    return binding_error(L, "read: binary file: " + path);
                }
                content += line;
                content.push_back('\n');
            }
        }
        if (!in.eof()) {
            return binding_error(L, "read: cannot read: " + path);
        }
        if (number == 0) {
            lua_pushlstring(L, "", 0);
            return 1;
        }
        if (number < first) {
            return binding_error(L,
                "read: first_line " + std::to_string(first)
                    + " exceeds file length " + std::to_string(number) + ": "
                    + path);
        }
        if (content.size() > MAX_OUTPUT_BYTES) {
            content.resize(MAX_OUTPUT_BYTES);
            content += "\n[truncated]";
        }
        lua_pushlstring(L, content.data(), content.size());
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
        const std::string path = lua_gettop(L) >= 1 && !lua_isnil(L, 1)
            ? luaL_checkstring(L, 1)
            : ".";
        int depth              = 1;
        if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
            depth = static_cast<int>(luaL_checkinteger(L, 2));
        }
        const bool show_hidden
            = lua_gettop(L) >= 3 && !lua_isnil(L, 3) && lua_toboolean(L, 3);
        if (depth < 1 || depth > MAX_LIST_DEPTH) {
            return binding_error(L,
                "list: depth must be between 1 and "
                    + std::to_string(MAX_LIST_DEPTH));
        }

        const ListDirectoryRequest request { path, depth, show_hidden };
        const GateOutcome gate = authorize_filesystem(L, request);
        if (!gate) {
            return binding_error(L, gate_denied(L, gate.denial, path));
        }
        const std::string target = filesystem_target(*gate.filesystem).string();

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
        constexpr auto timeout = std::chrono::seconds { 10 };
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
                return binding_error(
                    L, "grep: search timed out after 10 seconds");
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
                    return binding_error(
                        L, "grep: search timed out after 10 seconds");
                }
                const fs::directory_entry entry = *iterator;
                std::error_code status_error;
                const fs::file_status status
                    = entry.symlink_status(status_error);
                if (!status_error && fs::is_regular_file(status)) {
                    const GrepFileResult result
                        = grep_file(state, entry.path());
                    if (result == GrepFileResult::TIMED_OUT) {
                        return binding_error(
                            L, "grep: search timed out after 10 seconds");
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
        const std::string path    = lua_gettop(L) >= 1 && !lua_isnil(L, 1)
            ? luaL_checkstring(L, 1)
            : ".";
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
        const GateOutcome gate = authorize_filesystem(L, request);
        if (!gate) {
            return binding_error(L, gate_denied(L, gate.denial, path));
        }
        const fs::path& target = filesystem_target(*gate.filesystem);

        return grep_run(L, expression, target);
    }

    constexpr LuaBinding BINDINGS[] = {
        {
            "read",
            binding_read,
            R"desc(tool.read(path: string, first_line?: integer=1 last_line?: integer=nil) => string
Read the file at `path` returning its content.
first_line..last_line inclusive omit last_line to read to the end.
Fails on no such file, first_line past the end, last_line < first_line, or a
binary file. Over 64 KB is cut and marked "[truncated]".)desc",
        },
        {
            "list",
            binding_list,
            R"desc(tool.list(path?: string=".", depth?: integer=1, show_hidden?: bool=false) => FileEntry[]
List files and directories in `path`, returning their paths and sizes.
Paths are relative to the requested directory. Filename-sorted listing;
depth (1..5) descends into subdirectories and their entries come back flat.
`size` is absent for directories and "-" when unreadable.
Capped at 2000 entries.)desc",
        },
        {
            "grep",
            binding_grep,
            R"desc( tool.grep(path: string, pattern: string) => GrepHit[]
Run a POSIX extended regex (not a Lua pattern) over a file or directory tree,
one hit per matching line.
Capped at 500 hits, followed by a hit whose text is "[truncated]".)desc",
        },
    };

} // namespace

std::span<const LuaBinding> filesystem_lua_bindings() { return BINDINGS; }

} // namespace imza
