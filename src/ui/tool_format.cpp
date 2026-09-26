#include "ui/tool_format.h"
#include "common/util.h"
#include "conversation/format.h"
#include "network/json_io.h"
#include "tools/tool.h"

#include <algorithm>
#include <cctype>

namespace imza {

namespace {

    std::string subagent_args(const ToolCall& call)
    {
        const Json::Value parsed = parse_json(call.args);
        if (!parsed.isObject() || !parsed["tasks"].isArray()) {
            return call.args;
        }
        int research_count = 0;
        int build_count    = 0;
        for (const Json::Value& task : parsed["tasks"]) {
            if (!task.isObject() || !task["mode"].isString()) {
                continue;
            }
            if (task["mode"].asString() == "research") {
                ++research_count;
            }
            if (task["mode"].asString() == "build") {
                ++build_count;
            }
        }
        std::string summary;
        if (research_count > 0) {
            summary = std::to_string(research_count) + " research";
        }
        if (build_count > 0) {
            if (!summary.empty()) {
                summary += ", ";
            }
            summary += std::to_string(build_count)
                + (build_count == 1 ? " builder" : " builders");
        }
        return summary;
    }

    // Blockquote alert: renders bold red via the markdown sink's [!ERROR].
    std::string quoted_error(const std::string& text)
    {
        std::string out   = "**Error**\n> [!ERROR]\n";
        std::size_t start = 0;
        while (start < text.size()) {
            const std::size_t end = text.find('\n', start);
            const std::size_t last
                = end == std::string::npos ? text.size() : end;
            if (last > start) {
                out += "> " + text.substr(start, last - start) + "\n";
            }
            start = end == std::string::npos ? text.size() : end + 1;
        }
        return out;
    }

} // namespace

std::string tool_display_name(const std::string& name)
{
    if (name.empty()) {
        return name;
    }
    std::string out = name;
    out[0]
        = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

std::string tool_args_summary(const std::string& args)
{
    const Json::Value parsed = parse_json(args);
    if (!parsed.isObject() || parsed.empty()) {
        return args;
    }
    std::string out;
    for (const auto& key : parsed.getMemberNames()) {
        if (!out.empty()) {
            out += ' ';
        }
        out += key + "=";
        const Json::Value& value = parsed[key];
        if (value.isString()) {
            out += value.asString();
        } else if (value.isNull()) {
            out += "null";
        } else {
            out += write_json(value);
        }
    }
    return out;
}

std::string tool_call_head(const ToolCall& call)
{
    if (call.name == "skill") {
        const Json::Value parsed = parse_json(call.args);
        const std::string name   = json_string(parsed, "name");
        return name.empty() ? "Load Skill" : "Load Skill " + name;
    }
    if (call.name == "subagent") {
        return tool_display_name(call.name);
    }
    if (call.name == "lua") {
        const std::string counts = lua_dispatch_counts(call);
        return counts.empty() ? "Lua execution" : "Lua · " + counts;
    }
    std::string head       = tool_display_name(call.name);
    const std::string args = tool_args_summary(call.args);
    if (!args.empty()) {
        head += " " + args;
    }
    return head;
}

std::string tool_header_args(const ToolCall& call)
{
    if (call.name == "skill") {
        return json_string(parse_json(call.args), "name");
    }
    if (call.name == "subagent") {
        return subagent_args(call);
    }
    if (call.name == "lua") {
        // The binding counts identify the run; echoing the script would
        // spill it into the chat on failure.
        return lua_dispatch_counts(call);
    }
    return tool_args_summary(call.args);
}

std::string lua_dispatch_counts(const ToolCall& call)
{
    if (!call.result.has_value() || call.result->dispatch_log.empty()) {
        return "";
    }
    std::size_t total  = 0;
    std::size_t failed = 0;
    for (const LuaBindingCall& entry : call.result->dispatch_log) {
        ++total;
        failed += entry.ok ? 0 : 1;
    }
    std::string out = std::to_string(total) + (total == 1 ? " tool" : " tools");
    if (failed > 0) {
        out += " (" + std::to_string(failed) + " failed)";
    }
    return out;
}

std::string lua_dispatch_summary(const ToolCall& call)
{
    if (!call.result.has_value() || call.result->dispatch_log.empty()) {
        return "";
    }
    struct BindingCount {
        std::string name;
        std::size_t count  = 0;
        std::size_t failed = 0;
    };
    std::vector<BindingCount> counts;
    for (const LuaBindingCall& entry : call.result->dispatch_log) {
        auto found = std::find_if(
            counts.begin(), counts.end(), [&](const BindingCount& count) {
                return count.name == entry.binding;
            });
        if (found == counts.end()) {
            counts.push_back({ entry.binding, 0, 0 });
            found = std::prev(counts.end());
        }
        ++found->count;
        found->failed += entry.ok ? 0 : 1;
    }
    std::string summary;
    for (const BindingCount& count : counts) {
        if (!summary.empty()) {
            summary += " · ";
        }
        summary += std::to_string(count.count) + " " + count.name;
        if (count.failed > 0) {
            summary += " (" + std::to_string(count.failed) + " failed)";
        }
    }
    return summary;
}

ToolReport make_tool_report(const ToolCall& call)
{
    ToolReport report;
    report.summary = lua_dispatch_summary(call);
    report.detail  = lua_dispatch_counts(call);
    report.sections.push_back(
        ToolReportCode { "lua", json_string(parse_json(call.args), "script") });
    if (!call.result.has_value()) {
        return report;
    }
    // A return value can speak for itself; no need for an empty-output
    // placeholder when there is nothing printed. On failure the text is
    // the error itself, carried by the quoted block below.
    if (call.result->kind == ToolCall::Result::Kind::OUTPUT
        && !call.result->text.empty()) {
        report.sections.push_back(ToolReportCode { "txt", call.result->text });
    }
    if (call.result->return_value) {
        const LuaReturnKind kind = lua_return_kind(*call.result->return_value);
        // Strings are free-form output: keep them out of the markdown
        // renderer, numbers/bools are safe as bare text.
        if (kind == LuaReturnKind::JSON
            || (kind == LuaReturnKind::SCALAR
                && call.result->return_value->isString())) {
            report.sections.push_back(
                ToolReportCode { kind == LuaReturnKind::JSON ? "json" : "txt",
                    format_lua_return(*call.result->return_value) });
        } else {
            report.sections.push_back(ToolReportMarkdown {
                format_lua_return(*call.result->return_value) });
        }
    }
    if (call.result->kind != ToolCall::Result::Kind::OUTPUT
        && !call.result->text.empty()) {
        report.sections.push_back(
            ToolReportMarkdown { quoted_error(call.result->text) });
    }
    for (std::size_t index = 0; index < call.result->diffs.size(); ++index) {
        report.sections.push_back(
            ToolReportDiff { index, &call.result->diffs[index] });
    }
    for (std::size_t index = 0; index < call.result->canvases.size(); ++index) {
        report.sections.push_back(
            ToolReportCanvas { index, &call.result->canvases[index] });
    }
    return report;
}

std::string tool_report_markdown(const ToolReport& report)
{
    std::size_t fence = 3;
    for (const ToolReportSection& section : report.sections) {
        const auto* code = std::get_if<ToolReportCode>(&section);
        if (code == nullptr) {
            continue;
        }
        std::size_t run = 0;
        for (const char c : code->content) {
            run   = c == '`' ? run + 1 : 0;
            fence = std::max(fence, run + 1);
        }
    }

    const std::string open(fence, '`');
    std::string out;
    if (!report.summary.empty()) {
        out += "Bindings: " + report.summary + "\n\n";
    }
    for (const ToolReportSection& section : report.sections) {
        if (const auto* code = std::get_if<ToolReportCode>(&section)) {
            out += open + code->language + "\n" + code->content + "\n" + open
                + "\n";
        } else if (const auto* md = std::get_if<ToolReportMarkdown>(&section)) {
            out += md->content + "\n";
        } else if (const auto* chart = std::get_if<ToolReportCanvas>(&section);
            chart != nullptr && chart->view != nullptr) {
            // The markdown viewer cannot draw; name the chart so the
            // report still accounts for it.
            out += "Chart: " + chart->view->title + "\n";
        }
    }
    return out;
}

std::string lua_viewer_content(const ToolCall& call)
{
    return tool_report_markdown(make_tool_report(call));
}

} // namespace imza
