#pragma once

#include <cstddef>
#include <string>

#include "subsystems/session.h"

namespace ursa {

// Display formatting for tool calls in the conversation UI.
std::string tool_display_name(const std::string& name);
std::string tool_args_summary(const std::string& args);
std::string tool_call_head(const ToolCall& call);
std::string tool_header_args(const ToolCall& call);
std::string tool_code_language(const ToolCall& call);
std::size_t read_start_line(const ToolCall& call);

} // namespace ursa
