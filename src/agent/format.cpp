#include "format.h"

#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>

namespace ursa {

namespace {

} // namespace

std::string tool_display_name(const std::string& name)
{
    if (name.empty()) {
        return name;
    }
    std::string out = name;
    out[0] = static_cast<char>(
        std::toupper(static_cast<unsigned char>(out[0])));
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

namespace {

    std::string read_path(const ToolCall& call)
    {
        const Json::Value parsed = parse_json(call.args);
        if (parsed.isObject() && parsed["path"].isString()) {
            return parsed["path"].asString();
        }
        return call.args;
    }

} // namespace

std::string tool_call_head(const ToolCall& call)
{
    if (call.name == "read" || call.name == "list") {
        return tool_display_name(call.name) + " " + read_path(call);
    }
    if (call.name == "ask") {
        const Json::Value parsed = parse_json(call.args);
        int n = 0;
        if (parsed.isObject() && parsed["questions"].isArray()) {
            n = static_cast<int>(parsed["questions"].size());
        }
        return tool_display_name(call.name) + " (" + std::to_string(n)
            + " question" + (n == 1 ? "" : "s") + ")";
    }
    std::string head = tool_display_name(call.name);
    const std::string args = tool_args_summary(call.args);
    if (!args.empty()) {
        head += " " + args;
    }
    return head;
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

std::string question_form_markdown(const QuestionForm& form)
{
    std::string md;
    for (const auto& c : form) {
        if (!md.empty()) {
            md += "\n\n";
        }
        md += "Question: \"" + c.prompt + "\"";
        for (const auto& opt : c.options) {
            md += "\n- " + opt;
        }
    }
    return md;
}

std::string modal_answer_markdown(const ModalAnswer& answer)
{
    std::string md = "User answered:";
    for (const auto& card : answer.cards) {
        md += "\n";
        if (!card.prompt.empty()) {
            md += "- **" + card.prompt + "**\n";
            std::string body;
            if (!card.free_text.empty()) {
                body = card.free_text;
            } else if (!card.selected.empty()) {
                body = "";
                for (size_t i = 0; i < card.selected.size(); ++i) {
                    if (i) {
                        body += ", ";
                    }
                    body += card.selected[i];
                }
            }
            md += "  " + (body.empty() ? "—" : body);
        } else {
            for (const auto& sel : card.selected) {
                md += "\n> " + sel;
            }
            if (!card.free_text.empty()) {
                md += "\n> " + card.free_text;
            }
        }
    }
    return md;
}

std::string ask_answer_markdown(const ModalAnswer& answer)
{
    std::string md;
    int n = 1;
    for (const auto& card : answer.cards) {
        if (!md.empty()) {
            md += "\n";
        }
        md += std::to_string(n++) + ". **" + card.prompt + "**\n";
        std::string body;
        for (size_t i = 0; i < card.selected.size(); ++i) {
            if (i) {
                body += ", ";
            }
            body += card.selected[i];
        }
        if (!card.free_text.empty()) {
            if (!body.empty()) {
                body += " ";
            }
            body += card.free_text;
        }
        if (body.empty()) {
            body = "—";
        }
        md += "> " + body;
    }
    return md;
}

} // namespace ursa
