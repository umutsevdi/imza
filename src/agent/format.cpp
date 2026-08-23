#include "format.h"

#include <string>
#include <string_view>

namespace ursa {

namespace {

} // namespace

std::string fence_language(const std::string& tool)
{
    if (tool == "bash" || tool == "sh" || tool == "shell") {
        return "bash";
    }
    return tool;
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

std::string tool_request_markdown(const ToolCallRequest& req)
{
    std::string md = "Requested to call:";
    md += "\n```" + fence_language(req.name) + "\n" + req.args + "\n```";
    return md;
}

std::string tool_call_markdown(const ToolCall& call)
{
    std::string md
        = tool_request_markdown(ToolCallRequest { call.name, call.args, "" });
    if (!call.result.has_value()) {
        return md;
    }
    switch (call.result->kind) {
    case ToolCall::Result::Kind::OUTPUT:
        md += "\n\n---\n\n" + call.result->text;
        break;
    case ToolCall::Result::Kind::REJECT:
        md += "\n\n---\n\nUser answered:\n> Rejected:";
        if (!call.result->text.empty()) {
            md += " " + call.result->text;
        }
        break;
    case ToolCall::Result::Kind::CANCEL: break;
    }
    return md;
}

} // namespace ursa
