#pragma once

#include <cstddef>
#include <string>
#include <variant>
#include <vector>

#include "conversation/session.h"

namespace imza {

struct ToolReportCode {
    std::string language;
    std::string content;
};

// Markdown-native content appended without a code fence so tables and
// plain lines render as markdown.
struct ToolReportMarkdown {
    std::string content;
};

struct ToolReportDiff {
    std::size_t index;
    const DiffView* view;
};

using ToolReportSection
    = std::variant<ToolReportCode, ToolReportMarkdown, ToolReportDiff>;

struct ToolReport {
    std::string summary;
    std::string detail;
    std::vector<ToolReportSection> sections;
};

ToolReport make_tool_report(const ToolCall& call);
std::string tool_report_markdown(const ToolReport& report);

// Display formatting for tool calls in the conversation UI.
std::string tool_display_name(const std::string& name);
std::string tool_args_summary(const std::string& args);
std::string tool_call_head(const ToolCall& call);
std::string tool_header_args(const ToolCall& call);

// Card detail for a lua tool call: the binding dispatch log collapsed
// to "N tools (M failed)"; empty when there are no logged calls.
std::string lua_dispatch_counts(const ToolCall& call);
// Viewer detail for a lua tool call: the dispatch log grouped by binding as
// "2 read · 3 list (1 failed)".
std::string lua_dispatch_summary(const ToolCall& call);
// Viewer body for a lua tool call: a markdown report with the script and
// its output in fenced sections.
std::string lua_viewer_content(const ToolCall& call);

} // namespace imza
