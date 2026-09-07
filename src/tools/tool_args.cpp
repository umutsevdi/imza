#include "network/json_io.h"
#include "tools/tool.h"

#include <filesystem>

namespace ursa {

namespace {

    std::optional<std::string> validate_positive(std::string_view tool,
        const Json::Value& arguments, const char* key, bool allow_zero)
    {
        if (!arguments.isMember(key)) {
            return std::nullopt;
        }
        const Json::Value& value = arguments[key];
        if (!value.isInt64()
            || value.asInt64() < static_cast<Json::Int64>(allow_zero ? 0 : 1)) {
            return std::string(tool) + ": " + key
                + (allow_zero ? " must be 0 or greater"
                              : " must be 1 or greater");
        }
        return std::nullopt;
    }

} // namespace

std::optional<TodoList> parse_todo_args(const Json::Value& args)
{
    if (!args.isObject() || !args["todos"].isArray()) {
        return std::nullopt;
    }
    TodoList list;
    for (const auto& entry : args["todos"]) {
        if (!entry.isObject() || !entry["content"].isString()
            || entry["content"].asString().empty()) {
            return std::nullopt;
        }
        TodoItem item;
        item.content = entry["content"].asString();
        if (entry["status"].isString()) {
            const std::string status = entry["status"].asString();
            if (status == "in_progress") {
                item.status = TodoItem::Status::IN_PROGRESS;
            } else if (status == "completed") {
                item.status = TodoItem::Status::COMPLETED;
            } else if (status == "cancelled") {
                item.status = TodoItem::Status::CANCELLED;
            } else if (status != "pending") {
                return std::nullopt;
            }
        }
        list.items.push_back(std::move(item));
    }
    return list;
}

std::optional<QuestionForm> parse_ask_args(const std::string& args)
{
    const Json::Value parsed = parse_json(args);
    if (!parsed.isObject() || !parsed["questions"].isArray()
        || parsed["questions"].empty()) {
        return std::nullopt;
    }
    QuestionForm form;
    for (const auto& q : parsed["questions"]) {
        QuestionCard card;
        if (!q.isObject() || !q["prompt"].isString()
            || q["prompt"].asString().empty()) {
            return std::nullopt;
        }
        card.prompt = q["prompt"].asString();
        if (q["options"].isArray()) {
            for (const auto& o : q["options"]) {
                if (o.isString()) {
                    card.options.push_back(o.asString());
                }
            }
        }
        if (q["multi"].isBool()) {
            card.multi = q["multi"].asBool();
        }
        if (q["free_text"].isBool()) {
            card.free_text = q["free_text"].asBool();
        }
        form.push_back(std::move(card));
    }
    if (form.empty()) {
        return std::nullopt;
    }
    return form;
}

std::optional<std::string> validate_filesystem_tool_arguments(
    std::string_view tool, const Json::Value& arguments)
{
    if (!arguments.isObject()) {
        return std::string(tool) + ": arguments must be an object";
    }
    const bool write = tool == "edit" || tool == "write";
    const char* key  = write ? "file_path" : "path";
    if (tool == "list") {
        if (arguments.isMember(key) && !arguments[key].isNull()
            && !arguments[key].isString()) {
            return "list: path must be a string";
        }
    } else if (!arguments[key].isString()
        || arguments[key].asString().empty()) {
        return std::string(tool) + ": " + key + " must be a non-empty string";
    }
    if (tool == "read") {
        if (auto error
            = validate_positive(tool, arguments, "line_begin", false)) {
            return error;
        }
        if (auto error
            = validate_positive(tool, arguments, "line_end", false)) {
            return error;
        }
        if (arguments.isMember("line_begin") && arguments.isMember("line_end")
            && arguments["line_end"].asInt64()
                < arguments["line_begin"].asInt64()) {
            return "read: line_end is before line_begin";
        }
        return std::nullopt;
    }
    if (tool == "list") {
        return std::nullopt;
    }
    if (tool == "edit") {
        if (!arguments["old_string"].isString()
            || arguments["old_string"].asString().empty()) {
            return "edit: old_string must be a non-empty string";
        }
        if (!arguments["new_string"].isString()) {
            return "edit: new_string must be a string";
        }
        if (auto error
            = validate_positive(tool, arguments, "replace_count", true)) {
            return error;
        }
        return validate_positive(tool, arguments, "offset", true);
    }
    if (tool != "write") {
        return std::string(tool) + ": unsupported filesystem tool";
    }
    if (!arguments["text"].isString()) {
        return "write: text must be a string";
    }
    if (arguments.isMember("overwrite") && !arguments["overwrite"].isBool()) {
        return "write: overwrite must be a boolean";
    }
    const bool overwrite = arguments.get("overwrite", false).asBool();
    if (!overwrite) {
        return validate_positive(tool, arguments, "line", true);
    }
    if (!arguments.isMember("line_begin") || !arguments.isMember("line_end")) {
        return "write: overwrite requires line_begin and line_end";
    }
    if (auto error = validate_positive(tool, arguments, "line_begin", true)) {
        return error;
    }
    if (auto error = validate_positive(tool, arguments, "line_end", true)) {
        return error;
    }
    const Json::Int64 begin = arguments["line_begin"].asInt64();
    const Json::Int64 end   = arguments["line_end"].asInt64();
    if (begin != 0 && end != 0 && end < begin) {
        return "write: line_end is before line_begin";
    }
    return std::nullopt;
}

std::string todo_summary(const TodoList& todo)
{
    static constexpr std::string_view marks[] = { "[ ]", "[→]", "[x]", "[-]" };
    std::string out;
    for (const auto& it : todo.items) {
        if (!out.empty()) {
            out += '\n';
        }
        out += marks[static_cast<std::size_t>(it.status)];
        out += ' ';
        out += it.content;
    }
    return out;
}

ProjectTarget classify_project_target(
    const std::string& name, const std::string& args)
{
    const Json::Value parsed = parse_json(args);
    if (!parsed.isObject()) {
        return ProjectTarget::INVALID;
    }
    const char* key = name == "edit" || name == "write" ? "file_path" : "path";
    std::string path;
    if (parsed[key].isString()) {
        path = parsed[key].asString();
    } else if (name == "list" && parsed[key].isNull()) {
        path = ".";
    } else {
        return ProjectTarget::INVALID;
    }
    if (path.empty()) {
        path = name == "list" ? "." : "";
    }
    if (path.empty()) {
        return ProjectTarget::INVALID;
    }
    std::error_code ec;
    const std::filesystem::path target = std::filesystem::weakly_canonical(
        std::filesystem::absolute(path), ec);
    if (ec) {
        return ProjectTarget::INVALID;
    }
    const bool exists = std::filesystem::exists(target, ec);
    if (ec) {
        return ProjectTarget::INVALID;
    }
    if ((name == "read" || name == "edit")
        && (!exists || !std::filesystem::is_regular_file(target, ec))) {
        return ProjectTarget::INVALID;
    }
    if (name == "list"
        && (!exists || !std::filesystem::is_directory(target, ec))) {
        return ProjectTarget::INVALID;
    }
    if (name == "write" && !exists) {
        if ((parsed["overwrite"].isBool() && parsed["overwrite"].asBool())
            || !std::filesystem::is_directory(target.parent_path(), ec)) {
            return ProjectTarget::INVALID;
        }
    }
    if (ec) {
        return ProjectTarget::INVALID;
    }
    const std::filesystem::path root = std::filesystem::weakly_canonical(
        std::filesystem::current_path(ec), ec);
    if (ec) {
        return ProjectTarget::INVALID;
    }
    if (target == root) {
        return ProjectTarget::INSIDE;
    }
    const auto rel = target.lexically_relative(root);
    if (rel.empty()) {
        return ProjectTarget::OUTSIDE;
    }
    return *rel.begin() == ".." ? ProjectTarget::OUTSIDE
                                : ProjectTarget::INSIDE;
}

} // namespace ursa
