#include "format.h"

#include <string>
#include <string_view>
#include <vector>

namespace ursa {

namespace {

    std::string fence_language(const std::string& tool)
    {
        if (tool == "bash" || tool == "sh" || tool == "shell") {
            return "bash";
        }
        return tool;
    }

} // namespace

std::string question_form_markdown(const QuestionForm& form)
{
    std::string md;
    for (const auto& c : form) {
        if (!md.empty()) {
            md += "\n\n";
        }
        md += "Question: \"" + c.prompt + "\"";
        for (const auto& opt : c.options) {
            md += "\n- [ ] " + opt;
        }
    }
    return md;
}

std::string modal_answer_markdown(const ModalAnswer& answer)
{
    std::string md = "User answered:";
    for (const auto& card : answer.cards) {
        for (const auto& sel : card.selected) {
            md += "\n> " + sel;
        }
        if (!card.free_text.empty()) {
            md += "\n> " + card.free_text;
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

} // namespace ursa
