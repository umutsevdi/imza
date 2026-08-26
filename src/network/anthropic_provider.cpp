#include "network.h"

namespace ursa {

namespace {

    Json::Value build(const ChatRequest& req)
    {
        Json::Value root;
        root["model"]       = req.model;
        root["stream"]      = true;
        root["temperature"] = req.temperature;
        if (req.thinking_budget) {
            root["max_tokens"] = static_cast<Json::UInt64>(
                *req.thinking_budget + 4096);
            Json::Value thinking(Json::objectValue);
            thinking["type"]          = "enabled";
            thinking["budget_tokens"] = static_cast<Json::UInt64>(
                *req.thinking_budget);
            root["thinking"] = thinking;
        } else {
            root["max_tokens"] = 4096;
        }

        Json::Value messages(Json::arrayValue);
        Json::Value system(Json::arrayValue);
        Json::Value tool_results(Json::arrayValue);

        auto flush_results = [&]() {
            if (tool_results.size() > 0) {
                Json::Value o;
                o["role"]    = "user";
                o["content"] = tool_results;
                messages.append(o);
                tool_results = Json::Value(Json::arrayValue);
            }
        };

        for (const auto& m : req.messages) {
            if (m.type == Message::Type::SYSTEM) {
                system.append(m.content);
                continue;
            }
            if (m.type == Message::Type::TOOL) {
                Json::Value block;
                block["type"]        = "tool_result";
                block["tool_use_id"] = m.tool_call_id;
                block["content"]     = m.content;
                tool_results.append(block);
                continue;
            }
            flush_results();
            Json::Value o;
            o["role"] = role_str(m.type);
            if (!m.tool_calls.empty() || !m.thinking.empty()) {
                Json::Value content(Json::arrayValue);
                for (const auto& tb : m.thinking) {
                    Json::Value block;
                    block["type"]      = "thinking";
                    block["thinking"]  = tb.text;
                    block["signature"] = tb.signature;
                    content.append(block);
                }
                if (!m.content.empty()) {
                    Json::Value text;
                    text["type"] = "text";
                    text["text"] = m.content;
                    content.append(text);
                }
                for (const auto& tc : m.tool_calls) {
                    Json::Value use;
                    use["type"]  = "tool_use";
                    use["id"]    = tc.id;
                    use["name"]  = tc.name;
                    use["input"] = parse_json(tc.args);
                    content.append(use);
                }
                o["content"] = content;
            } else {
                o["content"] = m.content;
            }
            messages.append(o);
        }
        flush_results();
        if (!system.empty()) {
            root["system"] = system;
        }
        if (!req.tools.empty()) {
            Json::Value tools(Json::arrayValue);
            for (const auto& t : req.tools) {
                Json::Value spec;
                spec["name"]         = t.name;
                spec["description"]  = t.description;
                spec["input_schema"] = t.parameters;
                tools.append(spec);
            }
            root["tools"] = tools;
        }
        root["messages"] = messages;
        return root;
    }

    std::vector<std::string> headers()
    {
        return {
            "Content-Type: application/json",
            "Accept: text/event-stream",
        };
    }

    void flush_pending_tools(ParseState& state, std::vector<StreamEvent>& outs)
    {
        for (auto& [index, acc] : state.tool_accums) {
            if (acc.name.empty()) {
                continue;
            }
            outs.push_back(make_tool_call_event(finish_accum(acc)));
        }
        state.tool_accums.clear();
    }

    void parse(ParseState& state, std::string_view event,
        std::string_view data, std::vector<StreamEvent>& outs)
    {
        if (event == "content_block_start") {
            const Json::Value root  = parse_json(data);
            const Json::Value& block = root["content_block"];
            const std::string type   = block.get("type", "").asString();
            if (type == "tool_use") {
                ToolAccum& acc = state.tool_accums[root.get("index", 0).asInt()];
                acc.id   = block.get("id", "").asString();
                acc.name = block.get("name", "").asString();
            } else if (type == "thinking") {
                ThinkingAccum& acc
                    = state.thinking_accums[root.get("index", 0).asInt()];
                acc.text = block.get("thinking", "").asString();
            }
            return;
        }
        if (event == "message_start") {
            const Json::Value root = parse_json(data);
            const Json::Value& msg = root["message"];
            if (msg.isObject()) {
                const Json::Value& usage = msg["usage"];
                if (usage.isObject()) {
                    state.usage.cached_read
                        = usage.get("cache_read_input_tokens", 0).asUInt64();
                    state.usage.cached_write
                        = usage.get("cache_creation_input_tokens", 0).asUInt64();
                    state.usage.prompt
                        = usage.get("input_tokens", 0).asUInt64()
                        + state.usage.cached_read
                        + state.usage.cached_write;
                    state.usage.total  = state.usage.prompt;
                }
            }
            return;
        }
        if (event == "message_delta") {
            const Json::Value root = parse_json(data);
            const Json::Value& usage = root["usage"];
            if (usage.isObject()) {
                state.usage.completion = usage.get("output_tokens", 0).asUInt64();
                state.usage.total
                    = state.usage.prompt + state.usage.completion;
            }
            return;
        }
        if (event == "content_block_delta") {
            const Json::Value root  = parse_json(data);
            const Json::Value& delta = root["delta"];
            const std::string type   = delta.get("type", "").asString();
            if (type == "text_delta") {
                outs.push_back(make_delta_event(delta.get("text", "").asString()));
            } else if (type == "input_json_delta") {
                auto it = state.tool_accums.find(root.get("index", 0).asInt());
                if (it != state.tool_accums.end()) {
                    it->second.args
                        += delta.get("partial_json", "").asString();
                }
            } else if (type == "thinking_delta") {
                const std::string text = delta.get("thinking", "").asString();
                const int index        = root.get("index", 0).asInt();
                ThinkingAccum& acc     = state.thinking_accums[index];
                acc.text += text;
                outs.push_back(make_reasoning_event(text));
            }
            return;
        }
        if (event == "content_block_stop") {
            const Json::Value root = parse_json(data);
            const int index        = root.get("index", 0).asInt();
            auto it                = state.tool_accums.find(index);
            if (it != state.tool_accums.end()) {
                const ToolCallRequest req = finish_accum(it->second);
                state.tool_accums.erase(it);
                outs.push_back(make_tool_call_event(req));
                return;
            }
            auto think_it = state.thinking_accums.find(index);
            if (think_it != state.thinking_accums.end()) {
                const std::string signature
                    = root["content_block"].get("signature", "").asString();
                think_it->second.signature = signature;
                outs.push_back(make_reasoning_event("", signature));
                state.thinking_accums.erase(think_it);
            }
            return;
        }
        if (event == "message_stop") {
            flush_pending_tools(state, outs);
            if (state.usage.prompt > 0 || state.usage.completion > 0
                || state.usage.total > 0) {
                outs.push_back(make_usage_event(state.usage));
            }
            outs.push_back(make_done_event());
            return;
        }
    }

} // namespace

    extern const Provider anthropic_provider = { build, headers, parse };

} // namespace ursa
