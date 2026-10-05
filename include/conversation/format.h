#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

#include "conversation/session.h"

namespace imza {

// Long enough that `body`'s own backtick runs cannot close it.
inline std::size_t fenced_width(std::string_view body, std::size_t width = 3)
{
    std::size_t run = 0;
    for (const char c : body) {
        run   = c == '`' ? run + 1 : 0;
        width = std::max(width, run + 1);
    }
    return width;
}
inline std::string code_fence(std::string_view body)
{
    return std::string(fenced_width(body), '`');
}

// Conversation-transcript text shared by the agent and the UI.
std::string question_form_markdown(const QuestionForm& form);
std::string modal_answer_markdown(const ModalAnswer& answer);

// Lua tool return values render the same way for the model transcript and
// the UI report: scalars and scalar lists print as plain markdown, record
// lists as markdown tables, other values as pretty JSON that needs a code
// fence.
enum class LuaReturnKind { SCALAR, SCALAR_LIST, TABLE, JSON };
LuaReturnKind lua_return_kind(const JsonValue& return_value);
std::string format_lua_return(const JsonValue& return_value);
std::string format_lua_result(
    std::string text, const std::optional<JsonValue>& return_value);
std::string tool_result_text(const ToolCall::Result& result);
std::string tool_result_text(const ToolCall& call);
std::string denial_text(const std::string& reason);
std::string shell_status_text(const ShellStatus& status);

// History message for an assistant turn; dialects that require reasoning
// replay carry the provider state with the turn.
Message assistant_message(
    std::string content, const AssistantTurn* turn, ApiStandard dialect);

} // namespace imza
