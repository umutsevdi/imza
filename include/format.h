#pragma once

#include <string>

#include "agent.h"

namespace ursa {

std::string question_form_markdown(const QuestionForm& form);
std::string modal_answer_markdown(const ModalAnswer& answer);
std::string tool_request_markdown(const ToolCallRequest& req);

std::string fence_language(const std::string& tool);
std::string tool_call_markdown(const ToolCall& call);

} // namespace ursa
