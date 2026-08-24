#include "network.h"

namespace ursa {

namespace {

    void append_tools(Json::Value& root, const ChatRequest& req)
    {
        if (req.tools.empty()) {
            return;
        }
        Json::Value tools(Json::arrayValue);
        for (const auto& t : req.tools) {
            Json::Value fn;
            fn["type"]        = "function";
            fn["function"]    = Json::Value(Json::objectValue);
            fn["function"]["name"] = t.name;
            fn["function"]["description"] = t.description;
            fn["function"]["parameters"] = t.parameters;
            tools.append(fn);
        }
        root["tools"]      = tools;
        root["tool_choice"] = "auto";
    }

    Json::Value build(const ChatRequest& req)
    {
        Json::Value root;
        root["model"]       = req.model;
        root["stream"]      = true;
        root["temperature"] = req.temperature;
        append_tools(root, req);

        Json::Value messages(Json::arrayValue);
        for (const auto& m : req.messages) {
            Json::Value o;
            o["role"]    = role_str(m.type);
            o["content"] = m.content;
            if (!m.tool_calls.empty()) {
                Json::Value calls(Json::arrayValue);
                for (const auto& tc : m.tool_calls) {
                    Json::Value c;
                    c["id"]       = tc.id;
                    c["type"]     = "function";
                    c["function"] = Json::Value(Json::objectValue);
                    c["function"]["name"] = tc.name;
                    c["function"]["arguments"] = tc.args;
                    calls.append(c);
                }
                o["tool_calls"] = calls;
            }
            if (m.type == Message::Type::TOOL) {
                o["tool_call_id"] = m.tool_call_id;
            }
            messages.append(o);
        }
        root["messages"] = messages;
        return root;
    }

    std::string endpoint() { return "/chat/completions"; }

    std::vector<std::string> headers(const std::string& key)
    {
        return {
            "Content-Type: application/json",
            "Accept: text/event-stream",
            "Authorization: Bearer " + key,
        };
    }

    void flush_tools(ParseState& state, std::vector<StreamEvent>& outs)
    {
        for (auto& [index, acc] : state.tool_accums) {
            if (acc.name.empty()) {
                continue;
            }
            ToolCallRequest req;
            req.name = acc.name;
            req.args = acc.args.empty() ? "{}" : acc.args;
            req.id   = acc.id;
            outs.push_back(make_tool_call_event(std::move(req)));
        }
        state.tool_accums.clear();
    }

    void take_delta(const Json::Value& delta, ParseState& state)
    {
        const Json::Value& tcs = delta["tool_calls"];
        if (!tcs.isArray()) {
            return;
        }
        for (const auto& tc : tcs) {
            ToolAccum& acc = state.tool_accums[tc.get("index", 0).asInt()];
            if (tc["id"].isString()) {
                acc.id = tc["id"].asString();
            }
            const Json::Value& fn = tc["function"];
            if (fn.isObject()) {
                if (fn["name"].isString() && !fn["name"].asString().empty()) {
                    acc.name = fn["name"].asString();
                }
                if (fn["arguments"].isString()) {
                    acc.args += fn["arguments"].asString();
                }
            }
        }
    }

    Status parse(ParseState& state, std::string_view, std::string_view data,
        std::vector<StreamEvent>& outs)
    {
        if (data == "[DONE]") {
            flush_tools(state, outs);
            outs.push_back(make_done_event());
            return Status::OK;
        }
        const Json::Value root = parse_json(data);
        if (root.isNull()) {
            outs.push_back(make_error_event(Status::JSON_ERROR));
            return Status::JSON_ERROR;
        }
        const Json::Value& choices = root["choices"];
        if (choices.isArray() && choices.size() > 0) {
            const Json::Value& delta = choices[0]["delta"];
            if (delta.isObject()) {
                if (delta["content"].isString()) {
                    outs.push_back(make_delta_event(delta["content"].asString()));
                }
                take_delta(delta, state);
            }
            if (choices[0]["finish_reason"].isString()) {
                flush_tools(state, outs);
                outs.push_back(make_done_event());
                return Status::OK;
            }
        }
        if (outs.empty()) {
            outs.push_back(make_delta_event(""));
        }
        return Status::OK;
    }

} // namespace

extern const Provider openai_provider = { build, endpoint, headers, parse };

} // namespace ursa
