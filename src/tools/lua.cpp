#include "tools/tool.h"

#include "network/json_io.h"
#include "permissions/evaluator.h"
#include "permissions/filesystem.h"
#include "platform/command_runner.h"
#include "workspace/environment.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace imza {

namespace {

    namespace fs = std::filesystem;

    constexpr std::size_t MAX_OUTPUT_BYTES = 64 * 1024;
    constexpr std::size_t MAX_MEMORY_BYTES = 256UL * 1024 * 1024;
    // Hook fires every N VM instructions to check the wall-clock deadline;
    // short scripts pay one clock read per interval.
    constexpr int HOOK_INTERVAL = 1000 * 1000;

    constexpr std::size_t MAX_LIST_ENTRIES = 2000;
    constexpr std::size_t MAX_GREP_ROWS    = 500;
    constexpr int MAX_LIST_DEPTH           = 5;

    struct ScriptRun {
        std::string output;
        std::size_t memory_used = 0;
        std::chrono::steady_clock::time_point deadline;
        bool truncated = false;
        std::vector<LuaBindingCall> log;
        // Borrowed; lives as long as the Tool that owns this VM run.
        const LuaHost* host = nullptr;
        bool has_rg         = false;
    };

    void* lua_alloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize)
    {
        auto* run = static_cast<ScriptRun*>(ud);
        if (nsize == 0) {
            std::free(ptr);
            run->memory_used -= osize;
            return nullptr;
        }
        const std::size_t held = ptr == nullptr ? 0 : osize;
        if (run->memory_used - held + nsize > MAX_MEMORY_BYTES) {
            return nullptr;
        }
        void* next = std::realloc(ptr, nsize);
        if (next != nullptr) {
            run->memory_used += nsize - held;
        }
        return next;
    }

    int lua_print(lua_State* L)
    {
        auto* run
            = static_cast<ScriptRun*>(lua_touserdata(L, lua_upvalueindex(1)));
        if (run->truncated) {
            return 0;
        }
        const int n = lua_gettop(L);
        for (int i = 1; i <= n && !run->truncated; ++i) {
            std::size_t len = 0;
            const char* s   = luaL_tolstring(L, i, &len);
            if (run->output.size() + len > MAX_OUTPUT_BYTES) {
                run->truncated = true;
                lua_pop(L, 1);
                break;
            }
            run->output.append(s, len);
            if (i < n) {
                run->output.push_back('\t');
            }
            lua_pop(L, 1);
        }
        run->output.push_back('\n');
        return 0;
    }

    void deadline_hook(lua_State* L, lua_Debug*)
    {
        auto* run = *static_cast<ScriptRun**>(lua_getextraspace(L));
        if (std::chrono::steady_clock::now() > run->deadline) {
            luaL_error(L, "execution time limit exceeded");
        }
    }

    // Text-only load: mode is the 3rd argument; force it to "t" so a
    // precompiled binary chunk cannot smuggle in code the source sandbox
    // never saw. The original loader is fetched from the registry because
    // luaB_load is static inside the amalgamation build.
    constexpr int LOAD_KEY = 'l';

    int text_only_load(lua_State* L)
    {
        lua_rawgeti(L, LUA_REGISTRYINDEX, LOAD_KEY);
        const auto* load
            = reinterpret_cast<lua_CFunction*>(lua_touserdata(L, -1));
        lua_pop(L, 1);
        const int top = lua_gettop(L);
        if (top >= 3) {
            lua_pushliteral(L, "t");
            lua_replace(L, 3);
        } else {
            while (lua_gettop(L) < 2) {
                lua_pushnil(L);
            }
            lua_pushliteral(L, "t");
        }
        return (*load)(L);
    }

    // Only sandbox-safe base libraries; tool access is via the bindings on
    // the `tool` table, each of which re-runs the filesystem permission
    // evaluation before touching the disk.
    void open_sandbox(lua_State* L, ScriptRun& run)
    {
        static const luaL_Reg loadedlibs[] = {
            { LUA_GNAME, luaopen_base },
            { LUA_COLIBNAME, luaopen_coroutine },
            { LUA_TABLIBNAME, luaopen_table },
            { LUA_STRLIBNAME, luaopen_string },
            { LUA_MATHLIBNAME, luaopen_math },
            { nullptr, nullptr },
        };
        for (const luaL_Reg* lib = loadedlibs; lib->func != nullptr; ++lib) {
            luaL_requiref(L, lib->name, lib->func, 1);
            lua_pop(L, 1);
        }
        lua_getglobal(L, LUA_GNAME);
        lua_pushnil(L);
        lua_setfield(L, -2, "dofile");
        lua_pushnil(L);
        lua_setfield(L, -2, "loadfile");
        lua_pushnil(L);
        lua_setfield(L, -2, "print");
        lua_pop(L, 1);
        lua_getglobal(L, LUA_STRLIBNAME);
        lua_pushnil(L);
        lua_setfield(L, -2, "dump");
        lua_pop(L, 1);

        // load stays available but is forced to text-only mode; see
        // text_only_load above.
        lua_getglobal(L, "load");
        auto* slot = static_cast<lua_CFunction*>(
            lua_newuserdatauv(L, sizeof(lua_CFunction), 0));
        *slot = lua_tocfunction(L, -2);
        lua_rawseti(L, LUA_REGISTRYINDEX, LOAD_KEY);
        lua_pop(L, 1);
        lua_pushcfunction(L, text_only_load);
        lua_setglobal(L, "load");

        *static_cast<ScriptRun**>(lua_getextraspace(L)) = &run;
        lua_pushlightuserdata(L, &run);
        lua_pushcclosure(L, lua_print, 1);
        lua_setglobal(L, "print");
    }

    // ---- bindings ---------------------------------------------------------

    ScriptRun* run_of(lua_State* L)
    {
        return *static_cast<ScriptRun**>(lua_getextraspace(L));
    }

    void record_call(
        lua_State* L, std::string_view binding, std::string target, bool ok)
    {
        run_of(L)->log.push_back(
            { std::string(binding), std::move(target), ok });
    }

    int binding_error(lua_State* L, std::string message)
    {
        lua_pushnil(L);
        lua_pushlstring(L, message.data(), message.size());
        return 2;
    }

    // The permission gate speaks Json args; the binding speaks the Lua
    // stack. This shim is the only place the two meet. ASK verdicts resolve
    // through the modal queue when an ask callback is wired, and collapse
    // to a rejection without one (unattended runs); the script sees the
    // outcome as nil,err either way. Calls that pass the gate land in the
    // dispatch log with their canonicalized target.
    std::optional<FilesystemRequest> evaluate(lua_State* L,
        std::string_view tool, const Json::Value& args,
        std::string_view binding = "")
    {
        ScriptRun* run = run_of(L);
        const std::string label
            = binding.empty() ? std::string(tool) : std::string(binding);
        const auto record = [&](bool ok, std::string target) {
            run->log.push_back({ label, std::move(target), ok });
        };
        if (run->host == nullptr || !run->host->context) {
            // No provider: trusted mode (tests, SKIP_PERMISSIONS paths).
            const std::string path = json_string(args, "path");
            record(true, fs::path(path).string());
            return FilesystemRequest { FilesystemRequest::Operation::READ,
                fs::path(path), args };
        }
        const FilesystemEvaluation evaluation = evaluate_filesystem_request(
            tool, write_json(args), run->host->context());
        switch (evaluation.decision.kind) {
        case PermissionDecision::Kind::ACCEPT:
            record(true, evaluation.request->target.string());
            return evaluation.request;
        case PermissionDecision::Kind::REJECT: return std::nullopt;
        case PermissionDecision::Kind::ASK: break;
        }
        if (!run->host->ask) {
            return std::nullopt;
        }
        ToolCallRequest request
            = evaluation.request->normalized_arguments.isObject()
            ? ToolCallRequest { std::string(tool),
                  write_json(evaluation.request->normalized_arguments), "" }
            : ToolCallRequest { std::string(tool), write_json(args), "" };
        request.permission_reason = evaluation.decision.reason;
        // Human think-time is free: shift the wall-clock deadline by the
        // paused duration so a slow approval does not consume the script's
        // execution budget.
        const auto paused_at     = std::chrono::steady_clock::now();
        const ModalResult result = run->host->ask(std::move(request)).get();
        run->deadline += std::chrono::steady_clock::now() - paused_at;
        const auto* verdict = std::get_if<ToolVerdict>(&result);
        if (verdict != nullptr
            && verdict->decision == ToolDecision::ACCEPT_ONCE) {
            record(true, evaluation.request->target.string());
            return evaluation.request;
        }
        return std::nullopt;
    }

    Json::Value string_arg(const char* key, const std::string& value)
    {
        Json::Value args;
        args[key] = value;
        return args;
    }

    // tool.read(path, first_line=1, last_line=nil) => string|nil, err
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

        Json::Value args   = string_arg("path", path);
        args["line_begin"] = static_cast<Json::Int64>(first);
        if (last_given) {
            args["line_end"] = static_cast<Json::Int64>(last);
        }
        const std::optional<FilesystemRequest> allowed
            = evaluate(L, "read", args);
        if (!allowed) {
            return binding_error(L, "read: permission denied: " + path);
        }
        const std::string target = allowed->target.string();

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

    // tool.list(path, depth=1, show_hidden=false) => [{path,type,size}]|nil,
    // err
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

        Json::Value args = string_arg("path", path);
        const std::optional<FilesystemRequest> allowed
            = evaluate(L, "list", args);
        if (!allowed) {
            return binding_error(L, "list: permission denied: " + path);
        }
        const std::string target = allowed->target.string();

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

    // tool.grep(path, pattern) => [{file, line, text}]|nil, err
    int tool_grep(lua_State* L)
    {
        const std::string path    = lua_gettop(L) >= 1 && !lua_isnil(L, 1)
            ? luaL_checkstring(L, 1)
            : ".";
        const std::string pattern = luaL_checkstring(L, 2);
        if (pattern.empty()) {
            return binding_error(L, "grep: pattern must be a non-empty string");
        }

        Json::Value args = string_arg("path", path);
        args["pattern"]  = pattern;
        const std::optional<FilesystemRequest> allowed
            = evaluate(L, "find", args, "grep");
        if (!allowed) {
            return binding_error(L, "grep: permission denied: " + path);
        }
        const std::string target = allowed->target.string();

        ScriptRun* run = run_of(L);
        return grep_run(L, pattern, target, run->has_rg);
    }

    // tool.todo.get() => [{content, status}]
    int tool_todo(lua_State* L)
    {
        ScriptRun* run = run_of(L);
        if (run->host == nullptr || !run->host->todo) {
            return binding_error(L, "todo.get: unavailable in this context");
        }
        const TodoList list = run->host->todo();
        record_call(L, "todo.get", "", true);
        lua_newtable(L);
        for (std::size_t i = 0; i < list.items.size(); ++i) {
            const TodoItem& item = list.items[i];
            lua_newtable(L);
            lua_pushlstring(L, item.content.data(), item.content.size());
            lua_setfield(L, -2, "content");
            const char* status = item.status == TodoItem::Status::PENDING
                ? "pending"
                : item.status == TodoItem::Status::IN_PROGRESS ? "in_progress"
                : item.status == TodoItem::Status::COMPLETED   ? "completed"
                                                               : "cancelled";
            lua_pushstring(L, status);
            lua_setfield(L, -2, "status");
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
        return 1;
    }

    // tool.todo.set(items) => true|nil, err
    // items: [{content = string, status = "pending"|"in_progress"|
    //                  "completed"|"cancelled"}]
    int tool_set_todo(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TTABLE);
        TodoList list;
        const std::size_t n = lua_rawlen(L, 1);
        for (std::size_t i = 1; i <= n; ++i) {
            lua_rawgeti(L, 1, static_cast<lua_Integer>(i));
            if (!lua_istable(L, -1)) {
                lua_pop(L, 1);
                return binding_error(L,
                    "todo.set: item " + std::to_string(i) + " must be a table");
            }
            lua_getfield(L, -1, "content");
            if (!lua_isstring(L, -1)) {
                lua_pop(L, 2);
                return binding_error(L,
                    "todo.set: item " + std::to_string(i)
                        + " needs a 'content' string");
            }
            const std::string content = lua_tostring(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, -1, "status");
            const char* status
                = lua_isstring(L, -1) ? lua_tostring(L, -1) : "pending";
            TodoItem item;
            item.content = content;
            if (std::string_view(status) == "in_progress") {
                item.status = TodoItem::Status::IN_PROGRESS;
            } else if (std::string_view(status) == "completed") {
                item.status = TodoItem::Status::COMPLETED;
            } else if (std::string_view(status) == "cancelled") {
                item.status = TodoItem::Status::CANCELLED;
            } else if (std::string_view(status) != "pending") {
                lua_pop(L, 2);
                return binding_error(L,
                    "todo.set: item " + std::to_string(i)
                        + " has unknown status '" + status + "'");
            }
            lua_pop(L, 1);
            lua_pop(L, 1);
            list.items.push_back(std::move(item));
        }
        ScriptRun* run = run_of(L);
        if (run->host == nullptr || !run->host->set_todo) {
            return binding_error(L, "todo.set: unavailable in this context");
        }
        run->host->set_todo(std::move(list));
        record_call(L, "todo.set", "", true);
        lua_pushboolean(L, 1);
        return 1;
    }

    // tool.ask(questions) => [{question, answer}]|nil, err
    // questions: [{prompt = string, options = {string...}?,
    //              multi = bool?, free_text = bool?}]
    int tool_ask(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TTABLE);
        QuestionForm form;
        const std::size_t n = lua_rawlen(L, 1);
        for (std::size_t i = 1; i <= n; ++i) {
            lua_rawgeti(L, 1, static_cast<lua_Integer>(i));
            if (!lua_istable(L, -1)) {
                lua_pop(L, 1);
                return binding_error(
                    L, "ask: card " + std::to_string(i) + " must be a table");
            }
            lua_getfield(L, -1, "prompt");
            if (!lua_isstring(L, -1)) {
                lua_pop(L, 2);
                return binding_error(L,
                    "ask: card " + std::to_string(i)
                        + " needs a 'prompt' string");
            }
            QuestionCard card;
            card.prompt = lua_tostring(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, -1, "options");
            if (lua_istable(L, -1)) {
                const std::size_t options = lua_rawlen(L, -1);
                for (std::size_t o = 1; o <= options; ++o) {
                    lua_rawgeti(L, -1, static_cast<lua_Integer>(o));
                    if (lua_isstring(L, -1)) {
                        card.options.emplace_back(lua_tostring(L, -1));
                    }
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
            lua_getfield(L, -1, "multi");
            card.multi = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            lua_getfield(L, -1, "free_text");
            card.free_text = lua_toboolean(L, -1) != 0;
            lua_pop(L, 2);
            form.push_back(std::move(card));
        }
        if (form.empty()) {
            return binding_error(L, "ask: at least one card is required");
        }

        ScriptRun* run = run_of(L);
        if (run->host == nullptr || !run->host->ask) {
            return binding_error(
                L, "ask: questions are unavailable in unattended runs");
        }
        const auto paused_at     = std::chrono::steady_clock::now();
        const ModalResult result = run->host->ask(std::move(form)).get();
        run->deadline += std::chrono::steady_clock::now() - paused_at;
        const auto* answer = std::get_if<ModalAnswer>(&result);
        if (answer == nullptr) {
            record_call(L, "ask", "", false);
            return binding_error(L, "ask: dismissed by the user");
        }
        record_call(L, "ask",
            std::to_string(answer->cards.size())
                + (answer->cards.size() == 1 ? " question" : " questions"),
            true);
        lua_newtable(L);
        for (std::size_t i = 0; i < answer->cards.size(); ++i) {
            const QuestionAnswer& qa = answer->cards[i];
            lua_newtable(L);
            lua_pushlstring(L, qa.prompt.data(), qa.prompt.size());
            lua_setfield(L, -2, "question");
            std::string joined = qa.free_text;
            for (std::size_t c = 0; c < qa.selected.size(); ++c) {
                if (c > 0) {
                    joined += ", ";
                }
                joined += qa.selected[c];
            }
            lua_pushlstring(L, joined.data(), joined.size());
            lua_setfield(L, -2, "answer");
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
        return 1;
    }

    // tool.skill(name) => string|nil, err
    int tool_skill(lua_State* L)
    {
        const std::string name = luaL_checkstring(L, 1);
        ScriptRun* run         = run_of(L);
        if (run->host == nullptr || !run->host->skills || !run->host->config
            || !run->host->skill_store) {
            return binding_error(L, "skill: unavailable in this context");
        }
        const std::vector<Skill> catalog = run->host->skills();
        Json::Value args;
        args["name"]                     = name;
        const std::optional<Skill> skill = resolve_skill(catalog, args);
        if (!skill) {
            record_call(L, "skill", name, false);
            return binding_error(L, "skill: unknown or unavailable skill");
        }
        if (skill_policy(run->host->config(), *skill) == SkillPolicy::DENY) {
            record_call(L, "skill", name, false);
            return binding_error(L, "skill: access denied by configuration");
        }
        record_call(L, "skill", name, true);
        SkillStore& store = run->host->skill_store();
        if (store.is_loaded(skill->path)) {
            const SkillRead already = read_skill(*skill);
            if (already.kind == SkillRead::Kind::OK) {
                lua_pushlstring(L, already.body.data(), already.body.size());
                return 1;
            }
        }
        const SkillRead read = read_skill(*skill);
        if (read.kind == SkillRead::Kind::READ_FAILED) {
            return binding_error(L, "skill: cannot read instructions");
        }
        if (read.kind == SkillRead::Kind::TOO_LARGE) {
            return binding_error(L, "skill: instructions exceed 128 KiB");
        }
        std::string error;
        if (!store.load(*skill, error)) {
            return binding_error(
                L, "skill: " + (error.empty() ? "cannot load" : error));
        }
        lua_pushlstring(L, read.body.data(), read.body.size());
        return 1;
    }

    // tool.sh(command, timeout=10, workspace=nil) => output, exit_code
    // (nil, err on failure)
    // Exactly the native shell permission flow, with one binding-local rule:
    // a single command expression per call. Chains (; || & and newlines)
    // must be composed in Lua. Everything else — redirects, expansions,
    // catalog lookups, session grants — is decided by evaluate_shell_request,
    // the same gate the native shell tool runs. `workspace` selects the
    // directory the command runs in; relative paths resolve against the
    // session working directory. The process cwd only moves through
    // Environment::chdir, so inheriting it as the default matches the
    // native shell tool without a syscall.
    int tool_sh(lua_State* L)
    {
        const std::string command = luaL_checkstring(L, 1);
        long timeout              = 10;
        if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
            timeout = std::clamp<long>(luaL_checkinteger(L, 2), 1, 120);
        }

        ScriptRun* run = run_of(L);
        if (run->host == nullptr || !run->host->shell_enabled) {
            return binding_error(
                L, "sh: shell access is disabled for this run");
        }

        std::string workspace;
        if (lua_gettop(L) >= 3 && !lua_isnil(L, 3)) {
            const std::string raw = luaL_checkstring(L, 3);
            if (raw.empty()) {
                return binding_error(L, "sh: workspace must not be empty");
            }
            fs::path dir(raw);
            if (dir.is_relative()) {
                // The session working directory lives in the workspace
                // snapshot; the process cwd already tracks it (Environment
                // chdirs the process), so this is plain path joining.
                const PermissionContext context = run->host->context
                    ? run->host->context()
                    : PermissionContext { };
                dir = (context.workspace ? context.workspace->working_directory
                                         : fs::current_path())
                    / dir;
            }
            std::error_code ec;
            if (!fs::is_directory(dir, ec)) {
                return binding_error(
                    L, "sh: workspace is not a directory: " + raw);
            }
            workspace = dir.string();
        }

        const ShellAnalysis analysis = analyze_shell(command);
        if (analysis.invocations.size() > 1) {
            return binding_error(L,
                "sh: one command per call; compose results in Lua instead of "
                "chaining with && || ; |");
        }
        if (analysis.invocations.empty()) {
            return binding_error(L, "sh: empty command");
        }

        const auto run_sh = [&]() -> int {
            CommandResult r = run_command(
                command, std::chrono::seconds(timeout), fs::path(workspace));
            if (!r.spawned) {
                record_call(L, "sh", command, false);
                return binding_error(L, "sh: failed to execute command");
            }
            if (r.timed_out) {
                record_call(L, "sh", command, false);
                return binding_error(
                    L, "sh: timed out after " + std::to_string(timeout) + "s");
            }
            record_call(L, "sh", command, r.exit_code == 0);
            std::string output = std::move(r.output);
            if (output.size() > MAX_OUTPUT_BYTES) {
                output.resize(MAX_OUTPUT_BYTES);
                output += "\n[truncated]";
            }
            lua_pushlstring(L, output.data(), output.size());
            lua_pushinteger(L, r.exit_code);
            return 2;
        };

        if (run->host == nullptr || !run->host->context) {
            // No provider: trusted mode (tests, SKIP_PERMISSIONS paths).
            return run_sh();
        }

        Json::Value args;
        args["command"] = command;
        args["timeout"] = static_cast<Json::Int64>(timeout);
        ToolCallRequest request { "shell", write_json(args), { } };
        const PermissionEvaluation evaluation
            = evaluate_shell_request(request, run->host->context());
        if (evaluation.decision.kind == PermissionDecision::Kind::REJECT) {
            return binding_error(L, "sh: " + evaluation.decision.reason);
        }
        if (evaluation.decision.kind == PermissionDecision::Kind::ACCEPT
            || run->host->skip_permissions) {
            return run_sh();
        }
        if (!run->host->ask) {
            // Unattended: fail closed.
            return binding_error(
                L, "sh: permission requires approval in attended runs");
        }

        // ASK: block on the modal queue like the native flow; think-time is
        // free, so shift the wall-clock deadline by the paused duration.
        const auto paused_at     = std::chrono::steady_clock::now();
        const ModalResult result = run->host->ask(evaluation.request).get();
        run->deadline += std::chrono::steady_clock::now() - paused_at;
        const auto* verdict = std::get_if<ToolVerdict>(&result);
        if (verdict == nullptr) {
            return binding_error(L, "sh: permission dismissed");
        }
        if (verdict->decision == ToolDecision::REJECT) {
            return binding_error(L,
                "sh: "
                    + (verdict->reason.empty() ? "rejected" : verdict->reason));
        }
        if (verdict->decision == ToolDecision::ACCEPT_FOR_SESSION) {
            if (evaluation.session_grants.empty() || !run->host->install_grants
                || !run->host->install_grants(evaluation.session_grants)) {
                return binding_error(L, "sh: session approval is unavailable");
            }
        }

        // Re-evaluate before execution, mirroring the native run flow: the
        // grants above may have turned the request into an auto-accept.
        const PermissionEvaluation current
            = evaluate_shell_request(evaluation.request, run->host->context());
        if (current.decision.kind == PermissionDecision::Kind::REJECT
            || current.request.args != evaluation.request.args) {
            return binding_error(L, "sh: " + current.decision.reason);
        }
        return run_sh();
    }

    constexpr std::size_t MAX_WEB_CHARS = 40000;

    // Mirrors web.cpp's truncate_output: cap at a UTF-8 boundary, then mark.
    std::string truncate_web(std::string text)
    {
        if (text.size() <= MAX_WEB_CHARS) {
            return text;
        }
        std::string out(truncate_utf8(text, MAX_WEB_CHARS));
        out += "\n[truncated: showing first " + std::to_string(out.size())
            + " of the content]";
        return out;
    }

    bool web_enabled(lua_State* L)
    {
        ScriptRun* run = run_of(L);
        return run->host != nullptr && run->host->web_enabled;
    }

    // tool.webfetch(url) => string|nil, err
    int tool_webfetch(lua_State* L)
    {
        const std::string url = luaL_checkstring(L, 1);
        if (!web_enabled(L)) {
            return binding_error(
                L, "webfetch: web access is disabled for this run");
        }

        FetchedPage page;
        std::string detail;
        const Status st = fetch_url(url, page, detail);
        if (st == Status::INVALID_URL) {
            record_call(L, "webfetch", url, false);
            return binding_error(L, "webfetch: " + detail + ": " + url);
        }
        if (st == Status::NETWORK_ERROR) {
            record_call(L, "webfetch", url, false);
            return binding_error(L, "webfetch: request failed: " + url);
        }
        if (st != Status::OK) {
            record_call(L, "webfetch", url, false);
            return binding_error(L, "webfetch: " + detail + ": " + url);
        }
        record_call(L, "webfetch", url, true);

        std::string body  = page.body;
        std::size_t begin = 0;
        while (begin < body.size()
            && std::isspace(static_cast<unsigned char>(body[begin]))) {
            ++begin;
        }
        const std::string head = to_lower(body.substr(
            begin, std::min(body.size() - begin, std::size_t(200))));
        const bool is_html     = !body.empty() && body[begin] == '<'
            && (head.starts_with("<!doctype html") || head.starts_with("<html")
                || head.starts_with("<head") || head.starts_with("<body")
                || head.starts_with("<div") || head.starts_with("<p")
                || head.starts_with("<h1") || head.starts_with("<h2")
                || head.starts_with("<!doctype html public"));
        std::string text = is_html ? html_to_text(page.body) : page.body;
        if (trim(text).empty()) {
            return binding_error(
                L, "webfetch: no readable content at " + page.url);
        }
        const std::string out = truncate_web(std::move(text));
        lua_pushlstring(L, out.data(), out.size());
        return 1;
    }

    // tool.websearch(query, num_results=5) => string|nil, err
    int tool_websearch(lua_State* L)
    {
        const std::string query = luaL_checkstring(L, 1);
        int num_results         = 5;
        if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
            num_results = static_cast<int>(luaL_checkinteger(L, 2));
        }
        num_results = std::clamp(num_results, 1, 10);
        if (!web_enabled(L)) {
            return binding_error(
                L, "websearch: web access is disabled for this run");
        }

        std::string text;
        const Status st = web_search(query, num_results, text);
        if (st == Status::NETWORK_ERROR) {
            record_call(L, "websearch", query, false);
            return binding_error(
                L, "websearch: request failed for '" + query + "'");
        }
        if (st != Status::OK) {
            record_call(L, "websearch", query, false);
            return binding_error(
                L, "websearch: search request rejected for '" + query + "'");
        }
        record_call(L, "websearch", query, true);
        if (trim(text).empty()) {
            lua_pushliteral(
                L, "No search results found. Try a different query.");
            return 1;
        }
        const std::string out = truncate_web(std::move(text));
        lua_pushlstring(L, out.data(), out.size());
        return 1;
    }

    void open_bindings(lua_State* L)
    {
        lua_newtable(L);
        lua_pushcfunction(L, tool_read);
        lua_setfield(L, -2, "read");
        lua_pushcfunction(L, tool_list);
        lua_setfield(L, -2, "list");
        lua_pushcfunction(L, tool_grep);
        lua_setfield(L, -2, "grep");
        lua_newtable(L);
        lua_pushcfunction(L, tool_todo);
        lua_setfield(L, -2, "get");
        lua_pushcfunction(L, tool_set_todo);
        lua_setfield(L, -2, "set");
        lua_setfield(L, -2, "todo");
        lua_pushcfunction(L, tool_ask);
        lua_setfield(L, -2, "ask");
        lua_pushcfunction(L, tool_skill);
        lua_setfield(L, -2, "skill");
        lua_pushcfunction(L, tool_webfetch);
        lua_setfield(L, -2, "webfetch");
        lua_pushcfunction(L, tool_websearch);
        lua_setfield(L, -2, "websearch");
        lua_pushcfunction(L, tool_sh);
        lua_setfield(L, -2, "sh");
        lua_setglobal(L, "tool");
    }

    // ---- driver -----------------------------------------------------------

    ToolOutput lua_run(
        const Json::Value& args, const LuaHost& host, bool has_rg)
    {
        const std::string script = json_string(args, "script");
        if (script.empty()) {
            return tool_error("lua: expected a non-empty 'script' string");
        }
        long timeout = 10;
        if (const auto value = json_int(args, "timeout")) {
            timeout = std::clamp(*value, 1L, 120L);
        }

        ScriptRun run;
        run.deadline
            = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
        run.host   = &host;
        run.has_rg = has_rg;

        lua_State* L = lua_newstate(lua_alloc, &run);
        if (L == nullptr) {
            return tool_error("lua: cannot create VM");
        }
        open_sandbox(L, run);
        open_bindings(L);
        lua_sethook(L, deadline_hook, LUA_MASKCOUNT, HOOK_INTERVAL);

        // The log records what ran even when the script dies mid-flight, so
        // every exit path below carries it out.
        const auto finish = [&](ToolOutput out) {
            out.dispatch_log = std::move(run.log);
            lua_close(L);
            return out;
        };

        const int loaded
            = luaL_loadbufferx(L, script.data(), script.size(), "script", "t");
        if (loaded != LUA_OK) {
            return finish(
                tool_error("lua: " + std::string(lua_tostring(L, -1))));
        }
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            return finish(
                tool_error("lua: " + std::string(lua_tostring(L, -1))));
        }
        std::string output = std::move(run.output);
        if (run.truncated) {
            output += "\n[truncated]";
        }
        return finish({ ToolOutput::Kind::OUTPUT, std::move(output) });
    }

} // namespace

Tool make_lua_tool(LuaHost host, bool has_rg)
{
    ToolSpec spec;
    spec.name = "lua";
    spec.description
        = "Execute a Lua 5.4 script in a sandboxed VM with no network or "
          "process access. Base libraries: string, table, math, coroutine "
          "(io/os/package are absent). Bindings live on the global `tool` "
          "table; each returns its data on success or nil, err on failure. "
          "Signatures:\n"
          "tool.read(path: string, first_line?: integer=1, last_line?: "
          "integer=nil) -> string\n"
          "tool.list(path?: string='.', depth?: integer=1..5, show_hidden?: "
          "boolean=false) -> [{path: string, type: 'file'|'dir', size?: "
          "string}]\n"
          "tool.grep(path: string, pattern: string) -> [{file: string, line: "
          "integer, text: string}]  -- regex = POSIX ERE (rg: Rust regex), "
          "not Lua patterns\n"
          "tool.todo.get() -> [{content: string, status: 'pending'|"
          "'in_progress'|'completed'|'cancelled'}]\n"
          "tool.todo.set(items: [{content: string, status?: string}]) -> "
          "boolean\n"
          "tool.ask(cards: [{prompt: string, options?: string[], multi?: "
          "boolean=false, free_text?: boolean=false}]) -> [{question: "
          "string, answer: string}]  -- pauses for user input\n"
          "tool.skill(name: string) -> string\n"
          "tool.webfetch(url: string) -> string  -- requires web access\n"
          "tool.websearch(query: string, num_results?: integer=5, max 10) -> "
          "string  -- requires web access\n"
          "tool.sh(command: string, timeout?: integer=10, max 120, "
          "workspace?: string) -> output, exit_code  -- one command per "
          "call; chains are rejected, compose them in Lua; approval "
          "matches the shell tool; workspace selects the directory to run "
          "in (relative paths resolve against the session working "
          "directory)\n"
          "print(args...) writes to the returned output.";
    spec.parameters = parse_json(
        R"json({"type":"object","properties":{"script":{"type":"string","description":"Lua source code to execute"},"timeout":{"type":"integer","description":"maximum script execution time in seconds, excluding pauses for permission prompts (default 10, max 120)"}},"required":["script"]})json");
    return { std::move(spec),
        [host = std::move(host), has_rg](
            const Json::Value& args) { return lua_run(args, host, has_rg); } };
}

} // namespace imza
