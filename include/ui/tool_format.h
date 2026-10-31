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

struct ToolReportCanvas {
    std::size_t index;
    const CanvasView* view;
};

using ToolReportSection = std::variant<ToolReportCode, ToolReportMarkdown,
    ToolReportDiff, ToolReportCanvas>;

struct ToolReport {
    std::string summary;
    std::string detail;
    std::vector<ToolReportSection> sections;
};

ToolReport make_tool_report(const ToolCall& call);
std::string tool_report_markdown(const ToolReport& report);

std::string tool_display_name(const std::string& name);
std::string tool_args_summary(const std::string& args);
std::string tool_call_head(const ToolCall& call);
std::string tool_header_args(const ToolCall& call);
// Loader detection across roster generations: persisted transcripts
// from before the rename still say "skill" and "load".
bool is_skill_call(const std::string& name);
bool is_module_call(const std::string& name);

// Card detail for a lua tool call: the binding dispatch log collapsed
// to "N tools (M failed)"; empty when there are no logged calls.
std::string lua_dispatch_counts(const ToolCall& call);
// Card detail for a finished lua tool call: the dispatch log folded into
// the workspace buckets "2 searches · 3 reads · 4 edits · 1 command"; the
// four keep a fixed order, plan/ask/canvas/mcp follow by descending count.
// Bindings with no workspace impact are dropped; empty when nothing is left.
std::string lua_dispatch_breakdown(const ToolCall& call);
// Viewer detail for a lua tool call: the dispatch log grouped by binding as
// "2 read · 3 list (1 failed)".
std::string lua_dispatch_summary(const ToolCall& call);
// Viewer body for a lua tool call: a markdown report with the script and
// its output in fenced sections.
std::string lua_viewer_content(const ToolCall& call);

} // namespace imza
