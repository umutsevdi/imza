#pragma once

#include <optional>
#include <string>

#include "agent/subsystems/session.h"

namespace ursa {

// Conversation-transcript text shared by the agent and the UI.
std::string question_form_markdown(const QuestionForm& form);
std::string modal_answer_markdown(const ModalAnswer& answer);
std::string ask_answer_markdown(const ModalAnswer& answer);

std::string tool_result_text(const ToolCall& call);
std::string denial_text(const std::string& reason);
std::string shell_status_text(const ShellStatus& status);
std::string append_shell_status(
    std::string text, const std::optional<ShellStatus>& status);

} // namespace ursa
