#include "conversation/format.h"

#include "common/util.h"
#include "network/json.h"

#include <algorithm>
#include <set>
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

namespace {

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

} // namespace

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

    LuaReturnKind classify_return(const JsonValue& value)
    {
        if (!value.is_array()) {
            return value.is_object() ? LuaReturnKind::JSON
                                     : LuaReturnKind::SCALAR;
        }
        const auto& array = value.get<JsonValue::array_t>();
        if (array.empty()) {
            return LuaReturnKind::JSON;
        }
        const bool all_scalars = std::all_of(
            array.begin(), array.end(), [](const JsonValue& entry) {
                return !entry.is_object() && !entry.is_array();
            });
        if (all_scalars) {
            return LuaReturnKind::SCALAR_LIST;
        }
        const bool all_objects = std::all_of(array.begin(), array.end(),
            [](const JsonValue& entry) { return entry.is_object(); });
        return all_objects ? LuaReturnKind::TABLE : LuaReturnKind::JSON;
    }

} // namespace

LuaReturnKind lua_return_kind(const JsonValue& value)
{
    return classify_return(value);
}

namespace {

    std::string render_lua_return(const JsonValue& value, LuaReturnKind& render)
    {
        render = classify_return(value);
        switch (render) {
        case LuaReturnKind::SCALAR:
            return value.is_string() ? value.as<std::string>()
                                     : json_dump(value);
        case LuaReturnKind::JSON: return json_dump_pretty(value);
        default: break;
        }
        const auto& array = value.get<JsonValue::array_t>();
        if (render == LuaReturnKind::SCALAR_LIST) {
            std::string out;
            for (const JsonValue& entry : array) {
                out += entry.is_string() ? entry.as<std::string>()
                                         : json_dump(entry);
                out += '\n';
            }
            return out;
        }
        // A formal table: list of objects. Columns are the union of keys in
        // first-seen order; missing fields render as empty cells. A nested
        // value anywhere demotes the whole list back to JSON.
        std::vector<std::string> columns;
        std::set<std::string> seen;
        for (const JsonValue& entry : array) {
            if (!entry.is_object()) {
                continue;
            }
            for (const auto& [key, member] : entry.get<JsonValue::object_t>()) {
                if (member.is_object() || member.is_array()) {
                    render = LuaReturnKind::JSON;
                    return json_dump_pretty(value);
                }
                if (seen.insert(key).second) {
                    columns.push_back(key);
                }
            }
        }
        // Markdown tables do not support multi-line or pipe-bearing cells;
        // soften the break and escape the separator. Very wide cells are
        // capped so one long field cannot flatten the table.
        // A width of 0 means no cap; the escaped token is never split.
        const auto escape = [](std::string_view text, std::size_t max_width) {
            std::size_t width = 0;
            std::string out;
            std::size_t i = 0;
            while (i < text.size()) {
                const std::size_t length
                    = utf8_sequence_length(static_cast<unsigned char>(text[i]));
                if (text[i] == '\n') {
                    out += "<br>";
                    width += 3;
                } else if (text[i] == '|') {
                    out += "\\|";
                    width += 1;
                } else {
                    // Copy the whole sequence so the cap never splits a
                    // multi-byte character.
                    out.append(text.substr(i, length));
                    width += 1;
                }
                i += length;
                if (max_width > 0 && width >= max_width) {
                    out += "\u2026";
                    break;
                }
            }
            return out;
        };
        const auto cell = [&escape](const JsonValue* value) {
            const std::string text = value == nullptr || value->is_null() ? ""
                : value->is_string() ? value->as<std::string>()
                                     : json_dump(*value);
            return escape(text, 80);
        };
        std::string out;
        out += '|';
        for (const std::string& key : columns) {
            out += escape(key, 0);
            out += '|';
        }
        out += "\n|";
        for (std::size_t i = 0; i < columns.size(); ++i) {
            out += "---|";
        }
        out += '\n';
        for (const JsonValue& entry : array) {
            out += '|';
            for (const std::string& key : columns) {
                out += cell(find_member(entry, key));
                out += '|';
            }
            out += '\n';
            if (out.size() > MAX_OUTPUT_BYTES) {
                render = LuaReturnKind::JSON;
                return json_dump_pretty(value);
            }
        }
        return out;
    }

} // namespace

std::string format_lua_return(const JsonValue& value)
{
    LuaReturnKind render = LuaReturnKind::JSON;
    return render_lua_return(value, render);
}

std::string format_lua_result(
    std::string text, const std::optional<JsonValue>& return_value)
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
        const std::string open = code_fence(body);
        text += open + "json\n" + body + "\n" + open;
        return text;
    }
    if (render == LuaReturnKind::SCALAR) {
        if (return_value->is_string()) {
            // Free-form output must not be interpreted as markdown.
            const std::string open = code_fence(body);
            text += open + "\n" + body + "\n" + open;
            return text;
        }
        text += body;
        return text;
    }
    if (render == LuaReturnKind::SCALAR_LIST) {
        text += std::string_view(body).substr(0, body.size() - 1);
    } else {
        text += body;
    }
    return text;
}

std::string tool_result_text(const ToolCall::Result& result)
{
    switch (result.kind) {
    case ToolCall::Result::Kind::REJECT:
    case ToolCall::Result::Kind::CANCEL: return denial_text(result.text);
    case ToolCall::Result::Kind::OUTPUT:
    case ToolCall::Result::Kind::ERROR:
        return append_shell_status(
            format_lua_result(result.text, result.return_value),
            result.shell_status);
    }
    return "";
}

std::string tool_result_text(const ToolCall& call)
{
    return call.result.has_value() ? tool_result_text(*call.result) : "";
}

} // namespace imza
