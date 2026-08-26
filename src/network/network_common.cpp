#include "network.h"

#include <curl/curl.h>

#include <memory>

#include "util.h"

namespace ursa {

std::vector<std::string> auth_headers(AuthType auth, const std::string& key)
{
    if (auth == AuthType::NONE || key.empty()) {
        return { };
    }
    if (auth == AuthType::ANTHROPIC) {
        return { "x-api-key: " + key, "anthropic-version: 2023-06-01" };
    }
    return { "Authorization: Bearer " + key };
}

namespace {

    size_t append_body(char* ptr, size_t, size_t n, void* userdata)
    {
        static_cast<std::string*>(userdata)->append(ptr, n);
        return n;
    }

} // namespace

Status http_get(const std::string& url,
    const std::vector<std::string>& headers, long timeout_secs,
    std::string& body, long* http_code)
{
    static thread_local CURL* handle = curl_easy_init();
    if (!handle) {
        return Status::NETWORK_ERROR;
    }

    curl_slist* list = nullptr;
    for (const auto& h : headers) {
        list = curl_slist_append(list, h.c_str());
    }

    curl_easy_reset(handle);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, timeout_secs);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, timeout_secs);
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(handle, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, append_body);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &body);

    const CURLcode res = curl_easy_perform(handle);
    curl_slist_free_all(list);

    long code = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &code);
    if (http_code != nullptr) {
        *http_code = code;
    }
    if (res != CURLE_OK) {
        return Status::NETWORK_ERROR;
    }
    return Status::OK;
}

std::string write_json(const Json::Value& value)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

Json::Value parse_json(std::string_view text)
{
    static thread_local Json::CharReaderBuilder builder;
    static thread_local std::unique_ptr<Json::CharReader> reader(
        builder.newCharReader());
    Json::Value value;
    std::string err;
    if (text.empty()
        || !reader->parse(
            text.data(), text.data() + text.size(), &value, &err)) {
        return Json::Value::null;
    }
    return value;
}

ToolCallRequest finish_accum(const ToolAccum& acc)
{
    ToolCallRequest req;
    req.name = acc.name;
    req.args = acc.args.empty() ? "{}" : acc.args;
    req.id   = acc.id;
    return req;
}

const char* role_str(Message::Type type)
{
    switch (type) {
    case Message::Type::SYSTEM: return "system";
    case Message::Type::USER: return "user";
    case Message::Type::ASSISTANT: return "assistant";
    case Message::Type::TOOL: return "tool";
    }
    return "user";
}

StreamEvent make_delta_event(std::string text)
{
    StreamEvent ev;
    ev.kind = StreamEvent::Kind::CONTENT_DELTA;
    ev.text = std::move(text);
    return ev;
}

StreamEvent make_tool_call_event(ToolCallRequest request)
{
    StreamEvent ev;
    ev.kind      = StreamEvent::Kind::TOOL_CALL;
    ev.tool_call = std::move(request);
    return ev;
}

StreamEvent make_question_event(QuestionForm form)
{
    StreamEvent ev;
    ev.kind     = StreamEvent::Kind::QUESTION;
    ev.question = std::move(form);
    return ev;
}

StreamEvent make_done_event()
{
    StreamEvent ev;
    ev.kind = StreamEvent::Kind::DONE;
    return ev;
}

StreamEvent make_error_event(Status error, std::string message)
{
    StreamEvent ev;
    ev.kind  = StreamEvent::Kind::ERROR;
    ev.error = error;
    ev.text  = std::move(message);
    return ev;
}

StreamEvent make_usage_event(Usage usage)
{
    StreamEvent ev;
    ev.kind  = StreamEvent::Kind::USAGE;
    ev.usage = usage;
    return ev;
}

StreamEvent make_connected_event()
{
    StreamEvent ev;
    ev.kind = StreamEvent::Kind::CONNECTED;
    return ev;
}

StreamEvent make_reasoning_event(std::string text, std::string signature)
{
    StreamEvent ev;
    ev.kind              = StreamEvent::Kind::REASONING;
    ev.text              = std::move(text);
    ev.thinking_signature = std::move(signature);
    return ev;
}

Status parse_api_error(std::string_view body, std::string& message)
{
    message.clear();
    const Json::Value root = parse_json(body);
    if (root.isNull()) {
        return Status::OK;
    }
    std::string msg;
    std::string kind;
    const Json::Value& err = root["error"];
    if (err.isObject()) {
        if (err["message"].isString()) {
            msg = err["message"].asString();
        }
        if (err["type"].isString()) {
            kind = err["type"].asString();
        }
        if (err["code"].isString() && kind.empty()) {
            kind = err["code"].asString();
        }
    } else if (err.isString()) {
        msg = err.asString();
    } else if (root["message"].isString()) {
        msg = root["message"].asString();
    }
    if (msg.empty() && kind.empty()) {
        return Status::OK;
    }
    const std::string hay = to_lower(kind + " " + msg);
    Status st             = Status::API_ERROR;
    if (hay.find("rate") != std::string::npos
        || hay.find("too many") != std::string::npos) {
        st = Status::RATE_LIMITED;
    } else if (hay.find("quota") != std::string::npos
        || hay.find("balance") != std::string::npos
        || hay.find("credit") != std::string::npos
        || hay.find("insufficient") != std::string::npos
        || hay.find("billing") != std::string::npos) {
        st = Status::BUDGET_EXCEEDED;
    }
    message = std::move(msg);
    return st;
}

} // namespace ursa
