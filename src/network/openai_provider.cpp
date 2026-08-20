#include "network.hpp"

namespace ursa {

namespace {

    Json::Value build(const ChatRequest& req)
    {
        Json::Value root;
        root["model"]       = req.model;
        root["stream"]      = true;
        root["temperature"] = req.temperature;

        Json::Value messages(Json::arrayValue);
        for (const auto& m : req.messages) {
            Json::Value o;
            o["role"]    = role_str(m.type);
            o["content"] = m.content;
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

    Status parse(std::string_view, std::string_view data, StreamEvent& out)
    {
        if (data == "[DONE]") {
            out = StreamEvent { StreamEvent::Kind::DONE, "", Status::OK };
            return Status::OK;
        }
        const Json::Value root = parse_json(data);
        if (root.isNull()) {
            out = StreamEvent { StreamEvent::Kind::ERROR, "",
                Status::JSON_ERROR };
            return Status::JSON_ERROR;
        }
        const Json::Value& choices = root["choices"];
        if (choices.isArray() && choices.size() > 0) {
            const Json::Value& delta = choices[0]["delta"];
            if (delta.isObject() && delta["content"].isString()) {
                out = StreamEvent { StreamEvent::Kind::CONTENT_DELTA,
                    delta["content"].asString(), Status::OK };
                return Status::OK;
            }
            if (choices[0]["finish_reason"].isString()) {
                out = StreamEvent { StreamEvent::Kind::DONE, "", Status::OK };
                return Status::OK;
            }
        }
        out = StreamEvent { StreamEvent::Kind::CONTENT_DELTA, "", Status::OK };
        return Status::OK;
    }

} // namespace

extern const Provider openai_provider = { build, endpoint, headers, parse };

} // namespace ursa
