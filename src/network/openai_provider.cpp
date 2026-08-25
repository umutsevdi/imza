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
        root["stream_options"]["include_usage"] = true;
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

    std::vector<std::string> headers()
    {
        return {
            "Content-Type: application/json",
            "Accept: text/event-stream",
        };
    }

    void flush_tools(ParseState& state, std::vector<StreamEvent>& outs)
    {
        for (auto& [index, acc] : state.tool_accums) {
            if (acc.name.empty()) {
                continue;
            }
            outs.push_back(make_tool_call_event(finish_accum(acc)));
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

    Usage read_usage(const Json::Value& root)
    {
        Usage u;
        const Json::Value& top = root["usage"];
        if (top.isObject()) {
            u.prompt    = top.get("prompt_tokens", 0).asUInt64();
            u.completion = top.get("completion_tokens", 0).asUInt64();
            u.total     = top.get("total_tokens", 0).asUInt64();
            const Json::Value& details = top["prompt_tokens_details"];
            if (details.isObject()) {
                u.cached_read = details.get("cached_tokens", 0).asUInt64();
            }
            return u;
        }
        const Json::Value& choices = root["choices"];
        if (choices.isArray() && choices.size() > 0) {
            const Json::Value& cu = choices[0]["usage"];
            if (cu.isObject()) {
                u.prompt    = cu.get("prompt_tokens", 0).asUInt64();
                u.completion = cu.get("completion_tokens", 0).asUInt64();
                u.total     = cu.get("total_tokens", 0).asUInt64();
                const Json::Value& details = cu["prompt_tokens_details"];
                if (details.isObject()) {
                    u.cached_read = details.get("cached_tokens", 0).asUInt64();
                }
            }
        }
        return u;
    }

    void parse(ParseState& state, std::string_view, std::string_view data,
        std::vector<StreamEvent>& outs)
    {
        if (data == "[DONE]") {
            flush_tools(state, outs);
            outs.push_back(make_done_event());
            return;
        }
        const Json::Value root = parse_json(data);
        if (root.isNull()) {
            outs.push_back(make_error_event(Status::JSON_ERROR));
            return;
        }
        if (root.isMember("error")) {
            const Json::Value& e = root["error"];
            std::string msg;
            if (e.isObject() && e["message"].isString()) {
                msg = e["message"].asString();
            } else if (e.isString()) {
                msg = e.asString();
            }
            outs.push_back(make_error_event(Status::API_ERROR, msg));
            return;
        }
        const Usage u = read_usage(root);
        const Json::Value& choices = root["choices"];
        bool done = false;
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
                done = true;
            }
        }
        if (!state.usage_emitted
            && (u.prompt > 0 || u.completion > 0 || u.total > 0)) {
            state.usage_emitted = true;
            outs.push_back(make_usage_event(u));
        }
        if (done) {
            outs.push_back(make_done_event());
        } else if (outs.empty()) {
            outs.push_back(make_delta_event(""));
        }
    }

} // namespace

    extern const Provider openai_provider = { build, headers, parse };

} // namespace ursa
