#include "tools/bindings.h"

#include "common/modal.h"
#include "permissions/shell.h"
#include "permissions/shell_analysis.h"
#include "platform/command_runner.h"
#include "workspace/environment.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <utility>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace imza {
namespace {

    namespace fs = std::filesystem;

    int binding_todo_get(lua_State* L)
    {
        LuaRunContext* run = run_of(L);
        if (!run->host->todo) {
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

    int binding_todo_set(lua_State* L)
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
            const std::string_view status
                = lua_isstring(L, -1) ? lua_tostring(L, -1) : "pending";
            lua_pop(L, 1);
            TodoItem item;
            item.content = content;
            if (status == "in_progress") {
                item.status = TodoItem::Status::IN_PROGRESS;
            } else if (status == "completed") {
                item.status = TodoItem::Status::COMPLETED;
            } else if (status == "cancelled") {
                item.status = TodoItem::Status::CANCELLED;
            } else if (status == "pending") {
                item.status = TodoItem::Status::PENDING;
            } else {
                lua_pop(L, 1);
                return binding_error(L,
                    "todo.set: item " + std::to_string(i)
                        + " has unknown status '" + std::string(status) + "'");
            }
            lua_pop(L, 1);
            list.items.push_back(std::move(item));
        }
        LuaRunContext* run = run_of(L);
        if (!run->host->set_todo) {
            return binding_error(L, "todo.set: unavailable in this context");
        }
        run->host->set_todo(std::move(list));
        record_call(L, "todo.set", "", true);
        lua_pushboolean(L, 1);
        return 1;
    }

    int binding_ask(lua_State* L)
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

        LuaRunContext* run = run_of(L);
        if (!run->host->ask) {
            return binding_error(
                L, "ask: questions are unavailable in unattended runs");
        }
        const ModalResult result
            = ask_with_deadline_credit(*run, std::move(form));
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

    int binding_shell(lua_State* L)
    {
        const std::string command = luaL_checkstring(L, 1);
        long timeout              = 10;
        if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
            timeout = std::clamp<long>(luaL_checkinteger(L, 2), 1, 120);
        }

        std::string workspace;
        if (lua_gettop(L) >= 3 && !lua_isnil(L, 3)) {
            const std::string raw = luaL_checkstring(L, 3);
            if (raw.empty()) {
                return binding_error(L, "shell: workspace must not be empty");
            }
            fs::path dir(raw);
            if (dir.is_relative()) {
                // The session working directory lives in the workspace
                // snapshot; the process cwd already tracks it (Environment
                // chdirs the process), so this is plain path joining.
                LuaRunContext* run              = run_of(L);
                const PermissionContext context = run->host->permission_context
                    ? run->host->permission_context()
                    : PermissionContext { };
                dir = (context.workspace ? context.workspace->working_directory
                                         : fs::current_path())
                    / dir;
            }
            std::error_code ec;
            if (!fs::is_directory(dir, ec)) {
                return binding_error(
                    L, "shell: workspace is not a directory: " + raw);
            }
            workspace = dir.string();
        }

        const ShellAnalysis analysis = analyze_shell(command);
        if (analysis.invocations.size() > 1) {
            return binding_error(L,
                "shell: one command per call; compose results in Lua instead "
                "of "
                "chaining with && || ; |");
        }
        if (analysis.invocations.empty()) {
            return binding_error(L, "shell: empty command");
        }

        const GateOutcome gate = authorize_shell(L,
            ShellRequest {
                command, std::chrono::seconds(timeout), fs::path(workspace) });
        if (!gate) {
            return binding_error(L, gate.denial);
        }
        const std::string run_dir
            = gate.shell ? gate.shell->workspace.string() : workspace;

        CommandResult r = run_command(
            command, std::chrono::seconds(timeout), fs::path(run_dir));
        if (!r.spawned) {
            record_call(L, "shell", command, false);
            return binding_error(L, "shell: failed to execute command");
        }
        if (r.timed_out) {
            record_call(L, "shell", command, false);
            return binding_error(
                L, "shell: timed out after " + std::to_string(timeout) + "s");
        }
        record_call(L, "shell", command, r.exit_code == 0);
        std::string output = std::move(r.output);
        if (output.size() > MAX_OUTPUT_BYTES) {
            output.resize(MAX_OUTPUT_BYTES);
            output += "\n[truncated]";
        }
        lua_pushlstring(L, output.data(), output.size());
        lua_pushinteger(L, r.exit_code);
        return 2;
    }

    constexpr LuaMethod BINDINGS[] = {
        {
            "todo.get",
            binding_todo_get,
            R"desc(() => TodoItem[]
The session task list in display order; empty array when unset.)desc",
        },
        { "todo.set", binding_todo_set,
            R"desc((items: TodoItem[]) => true
Set todo items.
Replaces the entire list: get, modify, set the full array back.
`status` defaults to "pending"; any other value is rejected.)desc" },
        {
            "ask",
            binding_ask,
            R"desc((cards: AskCard[]) => AskAnswer[]
Asks the user one or more questions and returns their answers.
`options` offers a choice list, `multi` allows several picks,
`free_text` allows typed input; a card may combine them, and `answer` is the
typed text plus the selected labels joined with ", ".
nil, Err when dismissed. Unavailable in unattended runs.)desc",
        },
        {
            "shell",
            binding_shell,
            R"desc((command: string, timeout?: integer=10, workspace?: string) => output: string, exit_code: integer
Runs a single external command, returning its captured output (capped at 64 KB)
and exit status.
A non-zero exit_code is a successful call, so test exit_code rather than nil.
nil, Err means it could not start, timed out (1..120 s) or was denied.
Chains and pipelines are rejected. Compose results in Lua instead.
`workspace` is the directory the command runs in.)desc",
            LuaCapability::SHELL,
            "shell: shell access is disabled for this run",
        },
    };

} // namespace

std::span<const LuaMethod> core_lua_methods() { return BINDINGS; }

void register_core(LuaState& state)
{
    static constexpr std::string_view types[] = {
        "TodoStatus = 'pending' | 'in_progress' | 'completed' | 'cancelled'",
        "TodoItem = { content: string, status: TodoStatus }",
        "AskCard = { prompt: string, options?: string[], multi?: bool, "
        "free_text?: bool }",
        "AskAnswer = { question: string, answer: string }",
    };
    state.register_module({ true, "", "", types, core_lua_methods() });
}

} // namespace imza
