#include "network.h"

namespace ursa {

namespace {

    Json::Value build(const ChatRequest& req)
    {
        Json::Value root;
        root["model"]       = req.model;
        root["stream"]      = true;
        root["temperature"] = req.temperature;
        root["max_tokens"]  = 4096;

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
            if (!m.tool_calls.empty()) {
                Json::Value content(Json::arrayValue);
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

    std::string endpoint() { return "/v1/messages"; }

    std::vector<std::string> headers(const std::string& key)
    {
        return {
            "Content-Type: application/json",
            "Accept: text/event-stream",
            "x-api-key: " + key,
            "anthropic-version: 2023-06-01",
        };
    }

    Status parse(ParseState& state, std::string_view event,
        std::string_view data, std::vector<StreamEvent>& outs)
    {
        const std::string ev(event);
        if (ev == "content_block_start") {
            const Json::Value root  = parse_json(data);
            const Json::Value& block = root["content_block"];
            if (block.get("type", "").asString() == "tool_use") {
                ToolAccum& acc = state.tool_accums[root.get("index", 0).asInt()];
                acc.id   = block.get("id", "").asString();
                acc.name = block.get("name", "").asString();
            }
            return Status::OK;
        }
        if (ev == "content_block_delta") {
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
            }
            return Status::OK;
        }
        if (ev == "content_block_stop") {
            const Json::Value root = parse_json(data);
            const int index        = root.get("index", 0).asInt();
            auto it                = state.tool_accums.find(index);
            if (it != state.tool_accums.end()) {
                ToolCallRequest req;
                req.name = it->second.name;
                req.args = it->second.args.empty() ? "{}" : it->second.args;
                req.id   = it->second.id;
                state.tool_accums.erase(it);
                outs.push_back(make_tool_call_event(std::move(req)));
            }
            return Status::OK;
        }
        if (ev == "message_stop") {
            outs.push_back(make_done_event());
            return Status::OK;
        }
        return Status::OK;
    }

} // namespace

extern const Provider anthropic_provider = { build, endpoint, headers, parse };

} // namespace ursa
