#include "network/json_io.h"
#include "network/network.h"
#include "network/sse_parse.h"
#include "common/util.h"

#include <curl/curl.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <string_view>

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

    struct BodySink {
        std::string* body;
        std::size_t cap;
        bool hit_cap;
    };

    size_t append_body(char* ptr, size_t, size_t n, void* userdata)
    {
        auto* sink = static_cast<BodySink*>(userdata);
        if (sink->cap != 0 && sink->body->size() + n > sink->cap) {
            sink->hit_cap = true;
            return 0;
        }
        sink->body->append(ptr, n);
        return n;
    }

} // namespace

Status http_get(const std::string& url, const std::vector<std::string>& headers,
    long timeout_secs, std::string& body, long* http_code,
    const HttpGetOptions& opts)
{
    static thread_local CURL* handle = curl_easy_init();
    if (!handle) {
        return Status::NETWORK_ERROR;
    }

    curl_slist* list = nullptr;
    for (const auto& h : headers) {
        list = curl_slist_append(list, h.c_str());
    }

    BodySink sink { &body, opts.max_bytes, false };
    curl_easy_reset(handle);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, timeout_secs);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, timeout_secs);
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION,
        opts.max_redirs > 0 ? 1L : 0L);
    if (opts.max_redirs > 0) {
        curl_easy_setopt(handle, CURLOPT_MAXREDIRS, opts.max_redirs);
    }
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(handle, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, append_body);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &sink);

    const CURLcode res = curl_easy_perform(handle);
    curl_slist_free_all(list);

    long code = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &code);
    if (http_code != nullptr) {
        *http_code = code;
    }
    if (res != CURLE_OK) {
        if (res == CURLE_WRITE_ERROR && sink.hit_cap) {
            if (opts.truncated != nullptr) {
                *opts.truncated = true;
            }
            return Status::OK;
        }
        return Status::NETWORK_ERROR;
    }
    return Status::OK;
}

Status http_post(const std::string& url, const std::vector<std::string>& headers,
    const std::string& payload, long timeout_secs, std::string& body,
    long* http_code, long max_redirs)
{
    static thread_local CURL* handle = curl_easy_init();
    if (!handle) {
        return Status::NETWORK_ERROR;
    }

    curl_slist* list = nullptr;
    for (const auto& h : headers) {
        list = curl_slist_append(list, h.c_str());
    }

    BodySink sink { &body, 0, false };
    curl_easy_reset(handle);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, timeout_secs);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, timeout_secs);
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, max_redirs > 0 ? 1L : 0L);
    if (max_redirs > 0) {
        curl_easy_setopt(handle, CURLOPT_MAXREDIRS, max_redirs);
    }
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(handle, CURLOPT_POST, 1L);
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE,
        static_cast<long>(payload.size()));
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, append_body);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &sink);

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

extern const Provider openai_provider;
extern const Provider anthropic_provider;

namespace {

    constexpr std::size_t kRawCap = 16 * 1024;

    struct StreamCtx {
        const Provider* provider = nullptr;
        StreamCallback cb;
        ParseState parse_state;
        std::string buf;
        std::string event;
        std::string data;
        std::string raw;
        std::vector<StreamEvent> outs;
        int retry_after = 0;
        int http_status = 0;
        bool connected  = false;
    };

    void mark_connected(StreamCtx& ctx)
    {
        if (ctx.connected || ctx.http_status < 200 || ctx.http_status >= 300) {
            return;
        }
        ctx.connected = true;
        ctx.cb(make_connected_event());
    }

    size_t header_callback(char* ptr, size_t size, size_t nmemb, void* userdata)
    {
        auto* ctx          = static_cast<StreamCtx*>(userdata);
        const size_t total = size * nmemb;
        std::string_view line(ptr, total);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.remove_suffix(1);
        }
        if (line.starts_with("HTTP/")) {
            const auto sp = line.find(' ');
            if (sp != std::string_view::npos) {
                std::string_view v = line.substr(sp + 1);
                std::from_chars(
                    v.data(), v.data() + v.size(), ctx->http_status);
            }
        }
        constexpr std::string_view key = "retry-after:";
        if (line.size() > key.size()
            && std::equal(
                key.begin(), key.end(), line.begin(), [](char a, char b) {
                    return a == std::tolower(static_cast<unsigned char>(b));
                })) {
            std::string_view v = line.substr(key.size());
            while (!v.empty() && v.front() == ' ') {
                v.remove_prefix(1);
            }
            ctx->retry_after = 0;
            std::from_chars(v.data(), v.data() + v.size(), ctx->retry_after);
        }
        mark_connected(*ctx);
        return total;
    }

    void dispatch_block(StreamCtx& ctx)
    {
        if (ctx.event.empty() && ctx.data.empty()) {
            return;
        }
        ctx.outs.clear();
        ctx.provider->parse(ctx.parse_state, ctx.event, ctx.data, ctx.outs);
        for (auto& ev : ctx.outs) {
            ctx.cb(ev);
        }
        ctx.event.clear();
        ctx.data.clear();
    }

    void process_line(StreamCtx& ctx, std::string_view line)
    {
        if (line.empty()) {
            dispatch_block(ctx);
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
            ctx.event.assign(e.data(), e.size());
        }
    }

    size_t write_callback(char* ptr, size_t, size_t n, void* userdata)
    {
        auto* ctx = static_cast<StreamCtx*>(userdata);
        if (ctx->raw.size() < kRawCap) {
            ctx->raw.append(ptr, std::min(n, kRawCap - ctx->raw.size()));
        }
        mark_connected(*ctx);
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
            if (ctx->parse_state.terminal) {
                break;
            }
        }
        ctx->buf.erase(0, pos);
        return ctx->parse_state.terminal ? 0 : n;
    }

    int progress_callback(
        void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
    {
        const auto* req = static_cast<const ChatRequest*>(userdata);
        return req->interrupted && req->interrupted() ? 1 : 0;
    }

    Status classify_failure(
        long code, const std::string& raw, std::string& message)
    {
        Status st = parse_api_error(raw, message);
        if (code == 429) {
            st = Status::RATE_LIMITED;
        } else if (code == 402) {
            st = Status::BUDGET_EXCEEDED;
        } else if (st == Status::OK) {
            st = Status::API_ERROR;
        }
        if (message.empty()) {
            message = "HTTP " + std::to_string(code);
        }
        return st;
    }

    CURL* reuse_handle()
    {
        static thread_local CURL* handle = curl_easy_init();
        return handle;
    }

} // namespace

Status stream(const Route& route, const ChatRequest& req, StreamCallback cb,
    int* retry_after)
{
    const Provider& provider = get_provider(route);
    const std::string body   = write_json(provider.build(req));
    const std::string& url   = route.endpoint;

    std::vector<std::string> header_strs = provider.headers();
    for (auto& h : auth_headers(route.auth, route.api_key)) {
        header_strs.push_back(std::move(h));
    }
    curl_slist* list = nullptr;
    for (const auto& h : header_strs) {
        list = curl_slist_append(list, h.c_str());
    }

    StreamCtx ctx;
    ctx.provider = &provider;
    ctx.cb       = std::move(cb);
    CURL* curl   = reuse_handle();
    if (!curl) {
        curl_slist_free_all(list);
        return Status::NETWORK_ERROR;
    }

    char errbuf[CURL_ERROR_SIZE] = { };
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &req);

    const CURLcode res = curl_easy_perform(curl);

    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(list);

    if (res == CURLE_WRITE_ERROR && ctx.parse_state.terminal) {
        return Status::OK;
    }
    if (res != CURLE_OK) {
        if (res == CURLE_ABORTED_BY_CALLBACK && req.interrupted
            && req.interrupted()) {
            return Status::OK;
        }
        std::string detail(
            errbuf[0] != '\0' ? errbuf : curl_easy_strerror(res));
        ctx.cb(make_error_event(Status::NETWORK_ERROR, std::move(detail)));
        return Status::NETWORK_ERROR;
    }
    if (code >= 400) {
        std::string message;
        const Status st = classify_failure(code, ctx.raw, message);
        ctx.cb(make_error_event(st, std::move(message)));
        if (retry_after) {
            *retry_after = ctx.retry_after;
        }
        return st;
    }
    if (retry_after) {
        *retry_after = ctx.retry_after;
    }
    return Status::OK;
}

Provider get_provider(const Route& route)
{
    if (route.dialect == ApiStandard::ANTHROPIC) {
        return anthropic_provider;
    }
    return openai_provider;
}

ToolCallRequest finish_accum(const ToolAccum& acc)
{
    ToolCallRequest req;
    req.name = acc.name;
    req.args = acc.args.empty() ? "{}" : acc.args;
    req.id   = acc.id;
    return req;
}

std::vector<std::string> stream_headers()
{
    return {
        "Content-Type: application/json",
        "Accept: text/event-stream",
    };
}

void flush_tool_accums(ParseState& state, std::vector<StreamEvent>& outs)
{
    for (auto& [index, acc] : state.tool_accums) {
        if (acc.name.empty()) {
            continue;
        }
        outs.push_back(make_tool_call_event(finish_accum(acc)));
    }
    state.tool_accums.clear();
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
