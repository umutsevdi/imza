#include "tools/bindings.h"

#include "common/modal.h"
#include "common/util.h"
#include "permissions/shell.h"
#include "permissions/shell_analysis.h"
#include "platform/command_runner.h"
#include "workspace/environment.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
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
        return 0;
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
        const long timeout        = std::clamp<long>(
            opt_integer(L, 2).value_or(10), 1, MAX_SCRIPT_SECONDS);

        std::string workspace;
        if (const auto given = opt_string(L, 3)) {
            if (const int failed = resolve_workspace(L, given, workspace);
                failed != 0) {
                return failed;
            }
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

        // The command blocks the VM in a C call, during which the
        // instruction hook cannot fire; credit the elapsed wall clock back
        // to the compute budget so a long command does not kill the script.
        const auto started = std::chrono::steady_clock::now();
        CommandResult r    = run_command(
            command, std::chrono::seconds(timeout), fs::path(run_dir));
        credit_deadline(*run_of(L), std::chrono::steady_clock::now() - started);

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
        const std::string output
            = truncate_marked(std::move(r.output), MAX_OUTPUT_BYTES);
        lua_pushlstring(L, output.data(), output.size());
        lua_pushinteger(L, r.exit_code);
        return 2;
    }

    // Parks the VM thread in slices, crediting the paused time back to the
    // compute budget so a poll loop survives past the script's timeout.
    // Interrupt and the hard deadline end it; the hook cannot fire while
    // this blocks in a C call, so both are checked here.
    int binding_sleep(lua_State* L)
    {
        const long given = static_cast<long>(luaL_checkinteger(L, 1));
        if (given < 0) {
            return binding_error(L, "sleep: seconds must not be negative");
        }
        // Clamping keeps the seconds-to-millis conversion away from overflow.
        const long requested = std::min(given, MAX_TASK_SECONDS);
        LuaRunContext* run   = run_of(L);
        const auto started   = std::chrono::steady_clock::now();
        auto remaining       = std::chrono::milliseconds(requested * 1000);
        // Sliced so the checks below stay responsive during a long park.
        constexpr auto SLICE = std::chrono::milliseconds(25);
        while (remaining > std::chrono::milliseconds::zero()) {
            if (run->host->interrupted && run->host->interrupted()) {
                credit_deadline(
                    *run, std::chrono::steady_clock::now() - started);
                record_call(L, "sleep", "", false);
                return binding_error(L, "sleep: interrupted");
            }
            if (std::chrono::steady_clock::now() > run->hard_deadline) {
                credit_deadline(
                    *run, std::chrono::steady_clock::now() - started);
                record_call(L, "sleep", "", false);
                return binding_error(
                    L, "sleep: script exceeded its maximum wall-clock time");
            }
            const auto slice = std::min(remaining, SLICE);
            std::this_thread::sleep_for(slice);
            remaining -= slice;
        }
        credit_deadline(*run, std::chrono::steady_clock::now() - started);
        record_call(L, "sleep", "", true);
        return 0;
    }

    constexpr LuaMethod BINDINGS[] = {
        {
            "sleep",
            binding_sleep,
            R"desc((seconds: integer) throws
Parks the script for `seconds` seconds without spending its execution
budget, so a poll loop can wait past the script timeout. Wakes early and throws
when the turn is interrupted or the script exceeds its maximum wall-clock
time.)desc",
        },
        {
            "todo.get",
            binding_todo_get,
            R"desc(() returns TodoItem[]
The session task list in display order; empty array when unset.)desc",
        },
        { "todo.set", binding_todo_set,
            R"desc((items: TodoItem[]) throws
Set todo items.
Replaces the entire list: get, modify, set the full array back.
`status` defaults to "pending"; any other value is rejected.)desc" },
        {
            "ask",
            binding_ask,
            R"desc((cards: AskCard[]) returns AskAnswer[], throws
Asks the user one or more questions and returns their answers.
`options` offers a choice list, `multi` allows several picks,
`free_text` allows typed input; a card may combine them, and `answer` is the
typed text plus the selected labels joined with ", ".
Throws when dismissed by the user; unavailable in unattended runs.)desc",
        },
        {
            "shell",
            binding_shell,
            R"desc((command: string, timeout?: integer=10, workspace?: string) returns output: string, exit_code: integer, throws
Runs a single external command, returning its captured output (capped at 64 KB)
and exit status.
A non-zero exit_code is a successful call, so test exit_code rather than nil.
Throws when the command could not start, timed out (1..240 s) or was denied.
Chains and pipelines are rejected. Compose results in Lua instead.
`workspace` is the directory the command runs in.)desc",
            LuaCapability::SHELL,
            "shell: shell access is disabled for this run",
        },
    };

} // namespace

const LuaModule& core_module()
{
    static constexpr std::string_view types[] = {
        "TodoStatus = 'pending' | 'in_progress' | 'completed' | 'cancelled'",
        "TodoItem = { content: string, status: TodoStatus }",
        "AskCard = { prompt: string, options?: string[], multi?: bool, "
        "free_text?: bool }",
        "AskAnswer = { question: string, answer: string }",
    };
    static constexpr LuaModule MODULE { true, "", "", types, BINDINGS };
    return MODULE;
}

} // namespace imza
