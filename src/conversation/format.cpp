#include "conversation/format.h"

#include "network/json_io.h"
#include "tools/bindings.h"

#include <algorithm>
#include <string>
#include <type_traits>

namespace imza {

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

Message assistant_message(
    std::string content, const AssistantTurn* turn, ApiStandard dialect)
{
    Message message { Message::Type::ASSISTANT, std::move(content) };
    const bool preserve_anthropic_reasoning = dialect == ApiStandard::ANTHROPIC
        && turn != nullptr && !turn->reasoning.empty();
    const bool preserve_openai_reasoning
        = dialect == ApiStandard::OPENAI_RESPONSES && turn != nullptr
        && !turn->reasoning_signature.empty();
    if (preserve_anthropic_reasoning || preserve_openai_reasoning) {
        message.thinking.push_back(
            { turn->reasoning, turn->reasoning_signature });
    }
    return message;
}

namespace {

    LuaReturnKind classify_return(const Json::Value& value)
    {
        if (!value.isArray()) {
            return value.isObject() ? LuaReturnKind::JSON
                                    : LuaReturnKind::SCALAR;
        }
        if (value.empty()) {
            return LuaReturnKind::JSON;
        }
        const bool all_scalars = std::all_of(
            value.begin(), value.end(), [](const Json::Value& entry) {
                return !entry.isObject() && !entry.isArray();
            });
        return all_scalars ? LuaReturnKind::SCALAR_LIST : LuaReturnKind::TABLE;
    }

} // namespace

LuaReturnKind lua_return_kind(const Json::Value& value)
{
    return classify_return(value);
}

// Renders the JSON-encoded return value for display: scalars and scalar
// lists print directly, uniform record lists render as markdown tables,
// and everything else falls back to pretty JSON. `render` reports the
// chosen form so callers can pick fence handling.
std::string render_lua_return(const Json::Value& value, LuaReturnKind& render)
{
    render = classify_return(value);
    switch (render) {
    case LuaReturnKind::SCALAR: return value.asString();
    case LuaReturnKind::JSON: return write_pretty_json(value);
    default: break;
    }
    if (render == LuaReturnKind::SCALAR_LIST) {
        std::string out;
        for (const Json::Value& entry : value) {
            out += entry.asString();
            out += '\n';
        }
        return out;
    }
    // A formal table: list of objects. Columns are the union of keys in
    // first-seen order; missing fields render as empty cells. A nested
    // value anywhere demotes the whole list back to JSON.
    for (const Json::Value& entry : value) {
        for (const auto& key : entry.getMemberNames()) {
            if (entry[key].isObject() || entry[key].isArray()) {
                render = LuaReturnKind::JSON;
                return write_pretty_json(value);
            }
        }
    }
    Json::Value columns(Json::arrayValue);
    Json::Value seen(Json::objectValue);
    for (const Json::Value& entry : value) {
        for (const auto& key : entry.getMemberNames()) {
            if (!seen.isMember(key)) {
                seen[key] = true;
                columns.append(key);
            }
        }
    }
    // Markdown tables do not support multi-line or pipe-bearing cells;
    // soften the break and escape the separator. Very wide cells are
    // capped so one long field cannot flatten the table.
    const auto cell = [](const Json::Value& value) {
        const std::string text = value.isNull() ? "" : value.asString();
        std::size_t width      = 0;
        std::string out;
        for (const char c : text) {
            if (c == '\n') {
                out += "<br>";
                width += 3;
            } else if (c == '|') {
                out += "\\|";
                width += 1;
            } else {
                out += c;
                width += 1;
            }
            if (width >= 80) {
                out += "\u2026";
                break;
            }
        }
        return out;
    };
    std::string out;
    out += '|';
    for (const Json::Value& key : columns) {
        out += cell(key);
        out += '|';
    }
    out += "\n|";
    for (std::size_t i = 0; i < columns.size(); ++i) {
        out += "---|";
    }
    out += '\n';
    for (const Json::Value& entry : value) {
        out += '|';
        for (const Json::Value& key : columns) {
            out += cell(entry[key.asString()]);
            out += '|';
        }
        out += '\n';
        if (out.size() > MAX_OUTPUT_BYTES) {
            render = LuaReturnKind::JSON;
            return write_pretty_json(value);
        }
    }
    return out;
}

std::string format_lua_return(const Json::Value& value)
{
    LuaReturnKind render = LuaReturnKind::JSON;
    return render_lua_return(value, render);
}

std::string format_lua_result(
    std::string text, const std::optional<Json::Value>& return_value)
{
    if (!return_value.has_value()) {
        return text;
    }
    LuaReturnKind render   = LuaReturnKind::JSON;
    const std::string body = render_lua_return(*return_value, render);
    if (!text.empty()) {
        if (text.back() != '\n') {
            text += '\n';
        }
        text += '\n';
    }
    if (render == LuaReturnKind::JSON) {
        std::size_t fence = 3;
        std::size_t run   = 0;
        for (const char c : body) {
            run   = c == '`' ? run + 1 : 0;
            fence = std::max(fence, run + 1);
        }
        const std::string open(fence, '`');
        text += open + "json\n" + body + "\n" + open;
        return text;
    }
    if (render == LuaReturnKind::SCALAR) {
        if (return_value->isString()) {
            // Free-form output must not be interpreted as markdown.
            std::size_t fence = 3;
            std::size_t run   = 0;
            for (const char c : body) {
                run   = c == '`' ? run + 1 : 0;
                fence = std::max(fence, run + 1);
            }
            const std::string open(fence, '`');
            text += open + "\n" + body + "\n" + open;
            return text;
        }
        text += body;
        return text;
    }
    // SCALAR_LIST ends with a newline; TABLE does too. Keep the list
    // without its trailing newline to match scalar handling, keep the
    // table intact.
    if (render == LuaReturnKind::SCALAR_LIST) {
        text += std::string_view(body).substr(0, body.size() - 1);
    } else {
        text += body;
    }
    return text;
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
            format_lua_result(call.result->text, call.result->return_value),
            call.result->shell_status);
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
            md += "  " + (body.empty() ? "-" : body);
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
            body = "-";
        }
        md += "> " + body;
    }
    return md;
}

} // namespace imza
