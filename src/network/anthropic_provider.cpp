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
        for (const auto& m : req.messages) {
            if (m.type == Message::Type::SYSTEM) {
                system.append(m.content);
                continue;
            }
            Json::Value o;
            o["role"]    = role_str(m.type);
            o["content"] = m.content;
            messages.append(o);
        }
        if (!system.empty()) {
            root["system"] = system;
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

    Status parse(
        std::string_view event, std::string_view data, StreamEvent& out)
    {
        const std::string ev(event);
        if (ev == "content_block_delta") {
            const Json::Value root = parse_json(data);
            const std::string text
                = root.get("delta", Json::Value::null)["text"].asString();
            out = make_delta_event(text);
            return Status::OK;
        }
        if (ev == "message_stop") {
            out = make_done_event();
            return Status::OK;
        }
        out = make_delta_event("");
        return Status::OK;
    }

} // namespace

extern const Provider anthropic_provider = { build, endpoint, headers, parse };

} // namespace ursa
