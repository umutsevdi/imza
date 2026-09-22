#include "tools/bindings.h"

#include "common/modal.h"

#include <string>
#include <utility>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace imza {
namespace {

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

    constexpr LuaBinding BINDINGS[] = {
        {
            "todo.get",
            binding_todo_get,
            R"desc(tool.todo.get() => TodoItem[]
The session task list in display order; empty array when unset.)desc",
        },
        { "todo.set", binding_todo_set,
            R"desc(tool.todo.set(items: TodoItem[]) => true
Set todo items.
Replaces the entire list: get, modify, set the full array back.
`status` defaults to "pending"; any other value is rejected.)desc" },
        {
            "ask",
            binding_ask,
            R"desc(tool.ask(cards: AskCard[]) => AskAnswer[]
Asks the user one or more questions and returns their answers.
`options` offers a choice list, `multi` allows several picks,
`free_text` allows typed input; a card may combine them, and `answer` is the
typed text plus the selected labels joined with ", ".
nil, Err when dismissed. Unavailable in unattended runs.)desc",
        },
    };

} // namespace

std::span<const LuaBinding> session_lua_bindings() { return BINDINGS; }

} // namespace imza
