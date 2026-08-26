#pragma once

#include <string>

#include "agent.h"

namespace ursa {

std::string question_form_markdown(const QuestionForm& form);
std::string modal_answer_markdown(const ModalAnswer& answer);
std::string ask_answer_markdown(const ModalAnswer& answer);

std::string tool_display_name(const std::string& name);
std::string tool_args_summary(const std::string& args);
std::string tool_request_summary(const std::string& name,
    const std::string& args);
std::string tool_call_head(const ToolCall& call);
std::string tool_code_language(const ToolCall& call);
std::size_t read_start_line(const ToolCall& call);

} // namespace ursa
