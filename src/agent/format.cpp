#include "agent/format.h"

#include <string>
#include <type_traits>

namespace ursa {

std::string shell_status_text(const ShellStatus& status)
{
    return std::visit(
        [](const auto& value) -> std::string {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, ShellExit>) {
                return value.code == 0
                    ? ""
                    : "exited with code " + std::to_string(value.code);
            } else {
                return "timed out after "
                    + std::to_string(value.duration.count()) + "s";
            }
        },
        status);
}

std::string append_shell_status(
    std::string text, const std::optional<ShellStatus>& status)
{
    if (status.has_value()) {
        const std::string status_text = shell_status_text(*status);
        if (!status_text.empty()) {
            if (!text.empty() && text.back() != '\n') {
                text += '\n';
            }
            text += "[" + status_text + "]";
        }
    }
    return text;
}

std::string denial_text(const std::string& reason)
{
    if (reason.empty()) {
        return "user denied";
    }
    return "user denied: " + reason;
}

std::string tool_result_text(const ToolCall& call)
{
    if (!call.result.has_value()) {
        return "";
    }
    switch (call.result->kind) {
    case ToolCall::Result::Kind::REJECT:
    case ToolCall::Result::Kind::CANCEL: return denial_text(call.result->text);
    case ToolCall::Result::Kind::OUTPUT:
    case ToolCall::Result::Kind::ERROR:
        return append_shell_status(
            call.result->text, call.result->shell_status);
    }
    return "";
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
