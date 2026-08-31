#include "ui/tool_format.h"
#include "network/json_io.h"

#include <cctype>
#include <filesystem>

namespace ursa {

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

    int json_array_count(const std::string& args, std::string_view key)
    {
        const Json::Value parsed = parse_json(args);
        if (parsed.isObject() && parsed[std::string(key)].isArray()) {
            return static_cast<int>(parsed[std::string(key)].size());
        }
        return 0;
    }

    std::string plural_count(int n, std::string_view word)
    {
        std::string out = std::to_string(n) + ' ';
        out += word;
        if (n != 1) {
            out += 's';
        }
        return out;
    }

    std::string read_path(const ToolCall& call)
    {
        const Json::Value parsed = parse_json(call.args);
        if (parsed.isObject() && parsed["path"].isString()) {
            return parsed["path"].asString();
        }
        return call.args;
    }

    std::string tool_request_summary(
        const std::string& name, const std::string& args)
    {
        const Json::Value parsed = parse_json(args);
        const auto get_str       = [&](const char* key) -> std::string {
            if (parsed.isObject() && parsed[key].isString()) {
                return parsed[key].asString();
            }
            return "";
        };
        if (name == "edit") {
            std::string out
                = tool_display_name(name) + ": " + get_str("file_path");
            long offset = 0;
            if (parsed.isObject() && parsed["offset"].isIntegral()) {
                offset = parsed["offset"].asInt64();
            }
            if (offset > 0) {
                out += " · line " + std::to_string(offset);
            }
            return out;
        }
        if (name == "write") {
            const std::string path = get_str("file_path");
            bool overwrite = parsed.isObject() && parsed["overwrite"].isBool()
                && parsed["overwrite"].asBool();
            std::string out = tool_display_name(name) + ": " + path;
            if (!overwrite) {
                long line = 0;
                if (parsed.isObject() && parsed["line"].isIntegral()) {
                    line = parsed["line"].asInt64();
                }
                if (line > 0) {
                    out += " · below line " + std::to_string(line);
                }
            } else {
                long lb = 0;
                long le = 0;
                if (parsed.isObject()) {
                    if (parsed["line_begin"].isIntegral()) {
                        lb = parsed["line_begin"].asInt64();
                    }
                    if (parsed["line_end"].isIntegral()) {
                        le = parsed["line_end"].asInt64();
                    }
                }
                if (lb > 0 || le > 0) {
                    out += " · lines " + std::to_string(lb) + "-"
                        + std::to_string(le);
                }
            }
            return out;
        }
        if (name == "skill") {
            return "Load Skill " + get_str("name");
        }
        std::string head          = tool_display_name(name);
        const std::string summary = tool_args_summary(args);
        if (name == "todo") {
            return tool_display_name(name) + " ("
                + plural_count(json_array_count(args, "todos"), "task") + ")";
        }
        if (name == "ask") {
            return tool_display_name(name) + " ("
                + plural_count(json_array_count(args, "questions"), "question")
                + ")";
        }
        if (!summary.empty()) {
            head += " " + summary;
        }
        return head;
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
    if (call.name == "edit" || call.name == "write") {
        return tool_request_summary(call.name, call.args);
    }
    if (call.name == "read" || call.name == "list") {
        return read_path(call);
    }
    if (call.name == "shell") {
        return "shell";
    }
    if (call.name == "skill") {
        const Json::Value parsed = parse_json(call.args);
        if (parsed.isObject() && parsed["name"].isString()) {
            return "Load Skill " + parsed["name"].asString();
        }
        return "Load Skill";
    }
    if (call.name == "ask") {
        return tool_display_name(call.name) + " ("
            + plural_count(json_array_count(call.args, "questions"), "question")
            + ")";
    }
    if (call.name == "todo") {
        const int n = json_array_count(call.args, "todos");
        if (n == 0) {
            return tool_display_name(call.name);
        }
        return tool_display_name(call.name) + " (" + plural_count(n, "task")
            + ")";
    }
    if (call.name == "subagent") {
        return tool_display_name(call.name);
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
    if (call.name == "read" || call.name == "list") {
        return read_path(call);
    }
    if (call.name == "edit" || call.name == "write") {
        const Json::Value parsed = parse_json(call.args);
        std::string path;
        if (parsed.isObject()) {
            if (parsed["file_path"].isString()) {
                path = parsed["file_path"].asString();
            } else if (parsed["path"].isString()) {
                path = parsed["path"].asString();
            }
        }
        if (path.empty()) {
            path = call.args;
        }
        return path;
    }
    if (call.name == "shell") {
        const Json::Value parsed = parse_json(call.args);
        std::string cmd;
        if (parsed.isObject() && parsed["command"].isString()) {
            cmd = parsed["command"].asString();
        }
        if (cmd.empty()) {
            cmd = call.args;
        }
        return cmd;
    }
    if (call.name == "ask") {
        return plural_count(
            json_array_count(call.args, "questions"), "question");
    }
    if (call.name == "todo") {
        return plural_count(json_array_count(call.args, "todos"), "task");
    }
    if (call.name == "skill") {
        const Json::Value parsed = parse_json(call.args);
        return parsed.isObject() && parsed["name"].isString()
            ? parsed["name"].asString()
            : std::string { };
    }
    if (call.name == "subagent") {
        return subagent_args(call);
    }
    return tool_args_summary(call.args);
}

std::string tool_code_language(const ToolCall& call)
{
    if (call.name != "read") {
        return "";
    }
    std::string ext
        = std::filesystem::path(read_path(call)).extension().string();
    if (!ext.empty() && ext.front() == '.') {
        ext.erase(0, 1);
    }
    return ext;
}

std::size_t read_start_line(const ToolCall& call)
{
    const Json::Value parsed = parse_json(call.args);
    if (parsed.isObject() && parsed["line_begin"].isIntegral()) {
        const auto raw = parsed["line_begin"].asInt64();
        if (raw >= 1) {
            return static_cast<std::size_t>(raw);
        }
    }
    return 1;
}

} // namespace ursa
