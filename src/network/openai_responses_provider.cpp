#include "network/json_io.h"
#include "network/network.h"
#include "network/sse_parse.h"

namespace imza {

namespace {

    void append_tools(Json::Value& root, const ChatRequest& req)
    {
        if (req.tools.empty()) {
            return;
        }
        Json::Value tools(Json::arrayValue);
        for (const auto& tool : req.tools) {
            Json::Value spec;
            spec["type"]        = "function";
            spec["name"]        = tool.name;
            spec["description"] = tool.description;
            spec["parameters"]  = tool.parameters;
            tools.append(spec);
        }
        root["tools"]       = std::move(tools);
        root["tool_choice"] = "auto";
    }

    void append_message(Json::Value& input, const Message& message)
    {
        if (message.type == Message::Type::TOOL) {
            Json::Value output;
            output["type"]    = "function_call_output";
            output["call_id"] = message.tool_call_id;
            output["output"]  = message.content;
            input.append(std::move(output));
            return;
        }

        for (const ThinkingBlock& thinking : message.thinking) {
            if (thinking.signature.empty()) {
                continue;
            }
            Json::Value item;
            item["type"]              = "reasoning";
            item["encrypted_content"] = thinking.signature;
            item["summary"]           = Json::Value(Json::arrayValue);
            if (!thinking.text.empty()) {
                Json::Value summary;
                summary["type"] = "summary_text";
                summary["text"] = thinking.text;
                item["summary"].append(std::move(summary));
            }
            input.append(std::move(item));
        }

        if (!message.content.empty() || message.tool_calls.empty()) {
            Json::Value item;
            item["role"]    = role_str(message.type);
            item["content"] = message.content;
            input.append(std::move(item));
        }
        for (const ToolCallEntry& call : message.tool_calls) {
            Json::Value item;
            item["type"]      = "function_call";
            item["call_id"]   = call.id;
            item["name"]      = call.name;
            item["arguments"] = call.args;
            input.append(std::move(item));
        }
    }

    Json::Value build(const ChatRequest& req)
    {
        Json::Value root;
        root["model"]  = req.model;
        root["stream"] = true;
        root["store"]  = false;
        root["include"].append("reasoning.encrypted_content");
        if (req.reasoning_effort) {
            root["reasoning"]["effort"]  = *req.reasoning_effort;
            root["reasoning"]["summary"] = "auto";
        }
        if (req.max_output_tokens) {
            root["max_output_tokens"]
                = static_cast<Json::UInt64>(*req.max_output_tokens);
        }
        append_tools(root, req);

        Json::Value input(Json::arrayValue);
        for (const Message& message : req.messages) {
            append_message(input, message);
        }
        root["input"] = std::move(input);
        return root;
    }

    Usage read_usage(const Json::Value& response)
    {
        Usage usage;
        const Json::Value& value = response["usage"];
        if (!value.isObject()) {
            return usage;
        }
        usage.prompt               = value.get("input_tokens", 0).asUInt64();
        usage.completion           = value.get("output_tokens", 0).asUInt64();
        usage.total                = value.get("total_tokens", 0).asUInt64();
        const Json::Value& details = value["input_tokens_details"];
        if (details.isObject()) {
            usage.cached_read = details.get("cached_tokens", 0).asUInt64();
            usage.cached_write
                = details.get("cache_write_tokens", 0).asUInt64();
        }
        return usage;
    }

    void finish_response(ParseState& state, const Json::Value& response,
        std::vector<StreamEvent>& outs)
    {
        const Usage usage = read_usage(response);
        if (!state.usage_emitted
            && (usage.prompt > 0 || usage.completion > 0 || usage.total > 0)) {
            state.usage_emitted = true;
            outs.push_back(make_usage_event(usage));
        }
        state.terminal = true;
        flush_tool_accums(state, outs);
        outs.push_back(make_done_event());
    }

    void parse(ParseState& state, std::string_view event, std::string_view data,
        std::vector<StreamEvent>& outs)
    {
        const Json::Value root = parse_json(data);
        if (root.isNull()) {
            outs.push_back(make_error_event(Status::JSON_ERROR));
            return;
        }
        const std::string type
            = root.get("type", std::string(event)).asString();
        if (type == "response.output_text.delta"
            || type == "response.refusal.delta") {
            outs.push_back(make_delta_event(root.get("delta", "").asString()));
            return;
        }
        if (type == "response.reasoning_summary_text.delta"
            || type == "response.reasoning_text.delta") {
            outs.push_back(
                make_reasoning_event(root.get("delta", "").asString()));
            return;
        }
        if (type == "response.output_item.added") {
            const Json::Value& item = root["item"];
            if (item.get("type", "").asString() == "function_call") {
                ToolAccum& acc
                    = state.tool_accums[root.get("output_index", 0).asInt()];
                acc.id   = item.get("call_id", item.get("id", "")).asString();
                acc.name = item.get("name", "").asString();
                acc.args = item.get("arguments", "").asString();
            }
            return;
        }
        if (type == "response.function_call_arguments.delta") {
            state.tool_accums[root.get("output_index", 0).asInt()].args
                += root.get("delta", "").asString();
            return;
        }
        if (type == "response.output_item.done") {
            const int index             = root.get("output_index", 0).asInt();
            const Json::Value& item     = root["item"];
            const std::string item_type = item.get("type", "").asString();
            if (item_type == "function_call") {
                ToolAccum& acc = state.tool_accums[index];
                acc.id = item.get("call_id", item.get("id", acc.id)).asString();
                acc.name = item.get("name", acc.name).asString();
                if (item["arguments"].isString()) {
                    acc.args = item["arguments"].asString();
                }
                outs.push_back(make_tool_call_event(finish_accum(acc)));
                state.tool_accums.erase(index);
            } else if (item_type == "reasoning"
                && item["encrypted_content"].isString()) {
                outs.push_back(make_reasoning_event(
                    "", item["encrypted_content"].asString()));
            }
            return;
        }
        if (type == "response.completed" || type == "response.incomplete") {
            finish_response(state, root["response"], outs);
            return;
        }
        if (type == "response.failed" || type == "error") {
            state.terminal = true;
            const Json::Value& error
                = type == "response.failed" ? root["response"]["error"] : root;
            outs.push_back(make_error_event(
                Status::API_ERROR, error.get("message", "").asString()));
        }
    }

} // namespace

extern const Provider openai_responses_provider
    = { build, stream_headers, parse };

} // namespace imza
