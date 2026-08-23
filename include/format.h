#pragma once

#include <string>

#include "types.h"

namespace ursa {

std::string question_form_markdown(const QuestionForm& form);
std::string modal_answer_markdown(const ModalAnswer& answer);
std::string tool_request_markdown(const ToolCallRequest& req);

} // namespace ursa
