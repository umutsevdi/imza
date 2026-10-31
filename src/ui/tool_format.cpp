#include "ui/tool_format.h"
#include "common/util.h"
#include "conversation/format.h"
#include "network/json.h"
#include "network/json_io.h"
#include "tools/tool.h"

#include <algorithm>
#include <cctype>

namespace imza {

namespace {

    std::string subagent_args(const ToolCall& call)
    {
        const JsonValue parsed = parse_json(call.args);
        const JsonValue* tasks = find_member(parsed, "tasks");
        if (!parsed.is_object() || tasks == nullptr || !tasks->is_array()) {
            return call.args;
        }
        int research_count = 0;
        int build_count    = 0;
        for (const JsonValue& task : tasks->get<JsonValue::array_t>()) {
            const JsonValue* mode = find_member(task, "mode");
            if (!task.is_object() || mode == nullptr || !mode->is_string()) {
                continue;
            }
            const std::string value = mode->as<std::string>();
            if (value == "research") {
                ++research_count;
            }
            if (value == "build") {
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

    // Card-label buckets for a finished lua run: the four workspace
    // buckets in fixed order, then the rest by descending count. Bindings
    // absent from BINDING_BUCKETS are not counted.
    struct BucketLabel {
        std::string_view singular;
        std::string_view plural;
    };

    constexpr BucketLabel BUCKET_ORDER[] = {
        { "search", "searches" },
        { "read", "reads" },
        { "edit", "edits" },
        { "command", "commands" },
        { "plan", "plans" },
        { "ask", "asks" },
        { "canvas", "canvases" },
        { "mcp call", "mcp calls" },
    };
    constexpr std::size_t BUCKET_COUNT
        = sizeof(BUCKET_ORDER) / sizeof(BUCKET_ORDER[0]);
    constexpr std::size_t FIXED_BUCKETS = 4;

    constexpr std::size_t SEARCH  = 0;
    constexpr std::size_t READ    = 1;
    constexpr std::size_t EDIT    = 2;
    constexpr std::size_t COMMAND = 3;
    constexpr std::size_t PLAN    = 4;
    constexpr std::size_t ASK     = 5;
    constexpr std::size_t CANVAS  = 6;
    constexpr std::size_t MCP     = 7;

    struct BindingBucket {
        std::string_view binding;
        std::size_t bucket;
    };

    // Catalog paths of the current roster.
    constexpr BindingBucket BINDING_BUCKETS[] = {
        { "web.fetch", SEARCH },
        { "web.search", SEARCH },
        { "fs.read", READ },
        { "fs.list", READ },
        { "fs.grep", READ },
        { "fs.ts_query", READ },
        { "fs.insert", EDIT },
        { "fs.edit", EDIT },
        { "fs.write", EDIT },
        { "fs.mkdir", EDIT },
        { "fs.remove", EDIT },
        { "fs.copy", EDIT },
        { "fs.move", EDIT },
        { "shell", COMMAND },
        { "bg.shell", COMMAND },
        { "plan.get", PLAN },
        { "plan.create", PLAN },
        { "plan.edit", PLAN },
        { "ask", ASK },
        { "canvas.line", CANVAS },
        { "canvas.bar", CANVAS },
        { "canvas.pie", CANVAS },
        { "canvas.surface", CANVAS },
        { "mcp.call", MCP },
    };

    bool is_legacy_skill_name(const std::string& name)
    {
        return name == "skill";
    }

    bool is_legacy_module_name(const std::string& name)
    {
        return name == "load";
    }

} // namespace

// Loader calls across roster generations: pre-rename transcripts carry
// the bare wire names.
bool is_skill_call(const std::string& name)
{
    return name == "load_skill" || is_legacy_skill_name(name);
}

bool is_module_call(const std::string& name)
{
    return name == "load_module" || is_legacy_module_name(name);
}

std::string tool_display_name(const std::string& name)
{
    if (is_skill_call(name)) {
        return "Load Skill";
    }
    if (is_module_call(name)) {
        return "Load Module";
    }
    if (name == "load_mcp") {
        return "Load MCP";
    }
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
    const JsonValue parsed = parse_json(args);
    if (!parsed.is_object() || parsed.get<JsonValue::object_t>().empty()) {
        return args;
    }
    std::string out;
    for (const auto& [key, value] : parsed.get<JsonValue::object_t>()) {
        if (!out.empty()) {
            out += ' ';
        }
        out += key + "=";
        if (value.is_string()) {
            out += value.as<std::string>();
        } else if (value.is_null()) {
            out += "null";
        } else {
            out += imza::json_dump(value);
        }
    }
    return out;
}

std::string tool_call_head(const ToolCall& call)
{
    if (is_skill_call(call.name)) {
        SkillToolArgs parsed;
        (void)json_parse_checked(call.args, parsed);
        return parsed.name.empty() ? "Load Skill" : "Load Skill " + parsed.name;
    }
    if (call.name == "subagent") {
        return tool_display_name(call.name);
    }
    if (call.name == "load_mcp") {
        LoadMcpToolArgs parsed;
        (void)json_parse_checked(call.args, parsed);
        return parsed.server.empty() ? "Load MCP" : "Load MCP " + parsed.server;
    }
    if (is_module_call(call.name)) {
        LoadToolArgs parsed;
        (void)json_parse_checked(call.args, parsed);
        return parsed.name.empty() ? "Load Module"
                                   : "Load Module " + parsed.name;
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
    if (is_skill_call(call.name)) {
        SkillToolArgs parsed;
        (void)json_parse_checked(call.args, parsed);
        return parsed.name;
    }
    if (call.name == "subagent") {
        return subagent_args(call);
    }
    if (call.name == "load_mcp") {
        LoadMcpToolArgs parsed;
        (void)json_parse_checked(call.args, parsed);
        return parsed.server;
    }
    if (is_module_call(call.name)) {
        LoadToolArgs parsed;
        (void)json_parse_checked(call.args, parsed);
        return parsed.name;
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

std::string lua_dispatch_breakdown(const ToolCall& call)
{
    if (!call.result.has_value() || call.result->dispatch_log.empty()) {
        return "";
    }
    std::size_t counts[BUCKET_COUNT] = { };
    for (const LuaBindingCall& entry : call.result->dispatch_log) {
        for (const BindingBucket& rule : BINDING_BUCKETS) {
            if (rule.binding == entry.binding) {
                ++counts[rule.bucket];
                break;
            }
        }
    }
    std::vector<std::size_t> shown;
    std::vector<std::size_t> rest;
    for (std::size_t index = 0; index < BUCKET_COUNT; ++index) {
        if (counts[index] == 0) {
            continue;
        }
        (index < FIXED_BUCKETS ? shown : rest).push_back(index);
    }
    std::stable_sort(
        rest.begin(), rest.end(), [&](std::size_t left, std::size_t right) {
            return counts[left] > counts[right];
        });
    shown.insert(shown.end(), rest.begin(), rest.end());

    std::string out;
    for (const std::size_t index : shown) {
        if (!out.empty()) {
            out += " · ";
        }
        const BucketLabel& label = BUCKET_ORDER[index];
        out += std::to_string(counts[index]) + " "
            + std::string(counts[index] == 1 ? label.singular : label.plural);
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
    LuaToolArgs parsed;
    (void)json_parse_checked(call.args, parsed);
    report.sections.push_back(
        ToolReportCode { "lua", std::move(parsed.script) });
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
                && call.result->return_value->is_string())) {
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
        fence = fenced_width(code->content, fence);
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
