#include "tools/bindings.h"

#include "permissions/filesystem.h"
#include "platform/command_runner.h"
#include "workspace/environment.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace imza {
namespace {

    namespace fs = std::filesystem;

    constexpr std::size_t MAX_LIST_ENTRIES = 2000;
    constexpr std::size_t MAX_GREP_ROWS    = 500;
    constexpr int MAX_LIST_DEPTH           = 5;

    int tool_read(lua_State* L)
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
        const std::optional<FilesystemRequest> allowed = evaluate(L, request);
        if (!allowed) {
            return binding_error(L, "read: permission denied: " + path);
        }
        const std::string target = filesystem_target(*allowed).string();

        std::error_code ec;
        if (!fs::is_regular_file(fs::path(target), ec)) {
            return binding_error(L, "read: no such file: " + path);
        }
        std::ifstream in(target, std::ios::binary);
        if (!in) {
            return binding_error(L, "read: cannot open: " + path);
        }

        lua_Integer number = 0;
        std::size_t total  = 0;
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
        total = static_cast<std::size_t>(number);
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

    void push_list_entry(
        lua_State* L, const fs::directory_entry& entry, std::error_code& ec)
    {
        lua_newtable(L);
        const std::string name = entry.path().filename().string();
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

    int list_directory(lua_State* L, const std::string& target, int depth,
        bool show_hidden, int* count)
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
            push_list_entry(L, entry, sec);
            lua_rawseti(L, -2, static_cast<lua_Integer>(*count));
            if (depth > 1 && entry.is_directory(sec)) {
                if (const int failed = list_directory(L, entry.path().string(),
                        depth - 1, show_hidden, count)) {
                    return failed;
                }
            }
        }
        return 0;
    }

    int tool_list(lua_State* L)
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
        const std::optional<FilesystemRequest> allowed = evaluate(L, request);
        if (!allowed) {
            return binding_error(L, "list: permission denied: " + path);
        }
        const std::string target = filesystem_target(*allowed).string();

        std::error_code ec;
        if (!fs::is_directory(fs::path(target), ec)) {
            return binding_error(L, "list: no such directory: " + path);
        }

        lua_newtable(L);
        int count = 0;
        if (const int failed
            = list_directory(L, target, depth, show_hidden, &count)) {
            return failed;
        }
        return 1;
    }

    int grep_run(lua_State* L, const std::string& pattern,
        const std::string& path, bool has_rg)
    {
        const std::string command = has_rg
            ? "rg -n -- " + shell_quote(fs::path(pattern)) + " "
                + shell_quote(fs::path(path))
            : "grep -rsnE -- " + shell_quote(fs::path(pattern)) + " "
                + shell_quote(fs::path(path));
        constexpr auto timeout    = std::chrono::seconds { 10 };
        CommandResult result      = run_command(command, timeout);
        if (!result.spawned) {
            return binding_error(L, "grep: failed to start search command");
        }
        if (result.timed_out) {
            return binding_error(L, "grep: search timed out after 10 seconds");
        }
        if (result.exit_code > 1) {
            return binding_error(L,
                result.output.empty() ? "grep: search failed with exit code "
                        + std::to_string(result.exit_code)
                                      : "grep: " + result.output);
        }

        lua_newtable(L);
        int row           = 0;
        std::size_t start = 0;
        bool truncated    = false;
        // With a file target rg/grep print "line:text" (no filename);
        // with a directory they print "file:line:text".
        const bool directory   = fs::is_directory(fs::path(path));
        const char* fixed_file = directory ? "" : path.c_str();
        while (start < result.output.size()) {
            const std::size_t stop = result.output.find('\n', start);
            const std::string line = result.output.substr(start,
                stop == std::string::npos ? std::string::npos : stop - start);
            start = stop == std::string::npos ? result.output.size() : stop + 1;
            if (line.empty()) {
                continue;
            }
            std::string file;
            std::size_t number_begin = 0;
            const auto first         = line.find(':');
            const auto second        = first == std::string::npos
                ? std::string::npos
                : line.find(':', first + 1);
            if (directory && first != std::string::npos
                && second != std::string::npos) {
                number_begin = first + 1;
            }
            const std::size_t number_len
                = (directory ? second : first) - number_begin;
            if ((directory && second == std::string::npos)
                || number_len == std::string::npos
                || line.substr(number_begin, number_len)
                        .find_first_not_of("0123456789")
                    != std::string::npos) {
                continue;
            }
            if (++row > MAX_GREP_ROWS) {
                truncated = true;
                break;
            }
            lua_newtable(L);
            if (directory) {
                file = line.substr(0, first);
            } else {
                file = fixed_file;
            }
            lua_pushlstring(L, file.data(), file.size());
            lua_setfield(L, -2, "file");
            lua_pushinteger(
                L, std::stol(line.substr(number_begin, number_len)));
            lua_setfield(L, -2, "line");
            const std::string text
                = line.substr((directory ? second : first) + 1);
            lua_pushlstring(L, text.data(), text.size());
            lua_setfield(L, -2, "text");
            lua_rawseti(L, -2, static_cast<lua_Integer>(row));
        }
        if (truncated) {
            lua_newtable(L);
            lua_pushboolean(L, 0);
            lua_setfield(L, -2, "file");
            lua_pushinteger(L, 0);
            lua_setfield(L, -2, "line");
            lua_pushliteral(L, "[truncated]");
            lua_setfield(L, -2, "text");
            lua_rawseti(L, -2, static_cast<lua_Integer>(row));
        }
        return 1;
    }

    int tool_grep(lua_State* L)
    {
        const std::string path    = lua_gettop(L) >= 1 && !lua_isnil(L, 1)
            ? luaL_checkstring(L, 1)
            : ".";
        const std::string pattern = luaL_checkstring(L, 2);
        if (pattern.empty()) {
            return binding_error(L, "grep: pattern must be a non-empty string");
        }

        const FindFilesRequest request { path, pattern };
        const std::optional<FilesystemRequest> allowed
            = evaluate(L, request, "grep");
        if (!allowed) {
            return binding_error(L, "grep: permission denied: " + path);
        }
        const std::string target = filesystem_target(*allowed).string();

        LuaRunContext* run = run_of(L);
        return grep_run(L, pattern, target, run->has_rg);
    }

    constexpr LuaBinding BINDINGS[] = {
        {
            "read",
            tool_read,
            "tool.read(path: string, first_line?: integer=1, "
            "last_line?: integer=nil)\n    => string",
            "Read the file at `path` returning its content.\n"
            "first_line..last_line inclusive omit last_line to read to the "
            "end.\n"
            "Fails on no such file, first_line past the end, last_line < "
            "first_line, or a\n"
            "binary file. Over 64 KB is cut and marked \"[truncated]\".",
        },
        {
            "list",
            tool_list,
            "tool.list(path?: string=\".\", depth?: integer=1, "
            "show_hidden?: bool=false)\n    => FileEntry[]",
            "List files and directories in `path`.\n"
            "Filename-sorted listing; depth (1..5) descends into "
            "subdirectories and\n"
            "their entries come back flat, so join child names to their "
            "parent\n"
            "yourself. `size` is absent for directories and \"-\" when "
            "unreadable.\n"
            "Capped at 2000 entries.",
        },
        {
            "grep",
            tool_grep,
            "tool.grep(path: string, pattern: string) => GrepHit[]",
            "Run a POSIX extended regex (not a Lua pattern) over a file or\n"
            "directory tree, one hit per matching line.\n"
            "Capped at 500 hits, followed by a hit whose text is "
            "\"[truncated]\".",
        },
    };

} // namespace

std::span<const LuaBinding> filesystem_lua_bindings() { return BINDINGS; }

} // namespace imza
