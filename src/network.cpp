#include "network.hpp"

#include <curl/curl.h>
#include <json/json.h>

#include <cstring>
#include <string>
#include <vector>

namespace ursa {

namespace {

    const char* role_str(Message::Type type)
    {
        switch (type) {
        case Message::Type::SYSTEM: return "system";
        case Message::Type::USER: return "user";
        case Message::Type::ASSISTANT: return "assistant";
        }
        return "user";
    }

    std::string strip_slash(std::string_view base)
    {
        std::string out(base);
        while (!out.empty() && out.back() == '/') {
            out.pop_back();
        }
        return out;
    }

    std::string endpoint(Standard standard)
    {
        return standard == Standard::OPENAI ? std::string("/chat/completions")
                                            : std::string("/v1/messages");
    }

    std::vector<std::string> header_lines(
        Standard standard, const std::string& key)
    {
        std::vector<std::string> lines;
        lines.emplace_back("Content-Type: application/json");
        if (standard == Standard::OPENAI) {
            lines.push_back("Authorization: Bearer " + key);
        } else {
            lines.push_back("x-api-key: " + key);
            lines.emplace_back("anthropic-version: 2023-06-01");
        }
        return lines;
    }

    std::string write_json(const Json::Value& value)
    {
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        return Json::writeString(builder, value);
    }

    Json::Value parse_json(std::string_view text)
    {
        Json::Value value;
        Json::CharReaderBuilder reader;
        std::string err;
        const std::string copy(text);
        std::istringstream stream(copy);
        if (!Json::parseFromStream(reader, stream, &value, &err)) {
            return Json::Value::null;
        }
        return value;
    }

    // SSE framer state. Drives line processing with an enum phase (no
    // booleans).
    enum class SsePhase { Accumulating, Dispatch };

    struct StreamCtx {
        Standard standard;
        StreamCallback cb;
        std::string buf;
        std::string event;
        std::string data;
        SsePhase phase = SsePhase::Accumulating;
    };

    void dispatch_block(StreamCtx& ctx)
    {
        if (ctx.event.empty() && ctx.data.empty()) {
            return;
        }
        StreamEvent ev;
        parse_stream_event(ctx.standard, ctx.event, ctx.data, ev);
        ctx.cb(ev);
        ctx.event.clear();
        ctx.data.clear();
    }

    void process_line(StreamCtx& ctx, std::string_view line)
    {
        if (line.empty()) {
            ctx.phase = SsePhase::Dispatch;
            dispatch_block(ctx);
            ctx.phase = SsePhase::Accumulating;
            return;
        }
        if (line.starts_with("data:")) {
            std::string_view d = line.substr(5);
            if (!d.empty() && d.front() == ' ') {
                d = d.substr(1);
            }
            if (!ctx.data.empty()) {
                ctx.data += "\n";
            }
            ctx.data.append(d);
        } else if (line.starts_with("event:")) {
            std::string_view e = line.substr(6);
            if (!e.empty() && e.front() == ' ') {
                e = e.substr(1);
            }
            ctx.event = std::string(e);
        }
        // Other lines (comments starting with ':') are ignored.
    }

    size_t write_callback(char* ptr, size_t, size_t n, void* userdata)
    {
        auto* ctx = static_cast<StreamCtx*>(userdata);
        ctx->buf.append(ptr, n);

        size_t pos = 0;
        while (true) {
            const size_t nl = ctx->buf.find('\n', pos);
            if (nl == std::string::npos) {
                break;
            }
            std::string_view line(ctx->buf.data() + pos, nl - pos);
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            process_line(*ctx, line);
            pos = nl + 1;
        }
        ctx->buf.erase(0, pos);
        return n;
    }

} // namespace

Json::Value build_request(Standard standard, const ChatRequest& req)
{
    Json::Value root;
    root["model"]       = req.model;
    root["stream"]      = true;
    root["temperature"] = req.temperature;

    if (standard == Standard::OPENAI) {
        Json::Value messages(Json::arrayValue);
        for (const auto& m : req.messages) {
            Json::Value o;
            o["role"]    = role_str(m.type);
            o["content"] = m.content;
            messages.append(o);
        }
        root["messages"] = messages;
    } else {
        // Anthropic: system is top-level, messages hold only user/assistant.
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
        root["messages"]   = messages;
        root["max_tokens"] = 4096;
    }
    return root;
}

Status parse_stream_event(Standard standard, std::string_view event,
    std::string_view data, StreamEvent& out)
{
    if (standard == Standard::OPENAI) {
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

    // Anthropic: events are named; content arrives via content_block_delta.
    const std::string ev(event);
    if (ev == "content_block_delta") {
        const Json::Value root = parse_json(data);
        const std::string text
            = root.get("delta", Json::Value::null)["text"].asString();
        out = StreamEvent { StreamEvent::Kind::CONTENT_DELTA, text,
            Status::OK };
        return Status::OK;
    }
    if (ev == "message_stop") {
        out = StreamEvent { StreamEvent::Kind::DONE, "", Status::OK };
        return Status::OK;
    }
    out = StreamEvent { StreamEvent::Kind::CONTENT_DELTA, "", Status::OK };
    return Status::OK;
}

Status chat_stream(
    const Config& cfg, const ChatRequest& req, StreamCallback on_event)
{
    if (cfg.standard == Standard::ANTHROPIC) {
        return Status::UNSUPPORTED;
    }

    const std::string body = write_json(build_request(cfg.standard, req));
    const std::string url  = strip_slash(cfg.api_base) + endpoint(cfg.standard);

    const std::vector<std::string> headers
        = header_lines(cfg.standard, cfg.api_key);
    curl_slist* list = nullptr;
    for (const auto& h : headers) {
        list = curl_slist_append(list, h.c_str());
    }

    StreamCtx ctx { cfg.standard, on_event, { }, { }, { },
        SsePhase::Accumulating };

    CURL* curl = curl_easy_init();
    if (!curl) {
        curl_slist_free_all(list);
        return Status::NETWORK_ERROR;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

    const CURLcode res = curl_easy_perform(curl);

    curl_slist_free_all(list);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        return Status::NETWORK_ERROR;
    }
    return Status::OK;
}

} // namespace ursa
