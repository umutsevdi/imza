#pragma once

#include <optional>
#include <string>

#include "conversation/session.h"

namespace imza {

// Conversation-transcript text shared by the agent and the UI.
std::string question_form_markdown(const QuestionForm& form);
std::string modal_answer_markdown(const ModalAnswer& answer);
std::string ask_answer_markdown(const ModalAnswer& answer);

// How a lua return value renders for display: scalars and scalar lists
// print as plain markdown, record lists as markdown tables, other values
// as pretty JSON that needs a code fence.
enum class LuaReturnKind { SCALAR, SCALAR_LIST, TABLE, JSON };
LuaReturnKind lua_return_kind(const Json::Value& return_value);
// Single rendering of a lua tool's JSON return value for both the model
// transcript and the UI report: scalars print directly, uniform record
// lists render as markdown tables, other values as pretty JSON.
std::string format_lua_return(const Json::Value& return_value);
std::string format_lua_result(
    std::string text, const std::optional<Json::Value>& return_value);
std::string tool_result_text(const ToolCall& call);
std::string denial_text(const std::string& reason);
std::string shell_status_text(const ShellStatus& status);
std::string append_shell_status(
    std::string text, const std::optional<ShellStatus>& status);

// History message for an assistant turn; dialects that require reasoning
// replay carry the provider state with the turn.
Message assistant_message(
    std::string content, const AssistantTurn* turn, ApiStandard dialect);

} // namespace imza
