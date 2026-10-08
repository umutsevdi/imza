#define NOMINMAX
#include "common/util.h"
#include "network/json.h"
#include "network/json_io.h"
#include "network/network.h"
#include "network/sse_parse.h"

#include <curl/curl.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <optional>
#include <string_view>

namespace imza {

std::vector<std::string> auth_headers(
    AuthType auth, const std::string& key, const std::string& account_id)
{
    if (auth == AuthType::NONE || key.empty()) {
        return { };
    }
    if (auth == AuthType::ANTHROPIC) {
        return { "x-api-key: " + key, "anthropic-version: 2023-06-01" };
    }
    if (auth == AuthType::OPENAI_SUBSCRIPTION) {
        std::vector<std::string> headers = {
            "Authorization: Bearer " + key,
            "originator: codex_cli_rs",
        };
        if (!account_id.empty()) {
            headers.push_back("ChatGPT-Account-Id: " + account_id);
        }
        return headers;
    }
    return { "Authorization: Bearer " + key };
}

std::vector<std::string> request_headers(
    const Route& route, std::vector<std::string> base)
{
    if (!route.user_agent.empty()) {
        base.push_back(route.user_agent);
    }
    for (auto& h : auth_headers(route.auth, route.api_key, route.account_id)) {
        base.push_back(std::move(h));
    }
    return base;
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

    size_t capture_headers(char* ptr, size_t size, size_t nmemb, void* userdata)
    {
        auto* out          = static_cast<std::vector<std::string>*>(userdata);
        const size_t total = size * nmemb;
        std::string line(ptr, total);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        // Status lines (one per redirect hop) are not headers.
        if (!line.empty() && !line.starts_with("HTTP/")) {
            out->push_back(std::move(line));
        }
        return total;
    }

    struct CurlHandle {
        CURL* value = curl_easy_init();

        ~CurlHandle()
        {
            if (value != nullptr) {
                curl_easy_cleanup(value);
            }
        }
    };

    constexpr long CONNECT_TIMEOUT_SECS    = 10;
    constexpr long STALL_LIMIT_BYTES_PER_S = 1;
    // 5 minutes of silence aborts idle notification streams.
    constexpr long STALL_WINDOW_SECS = 300;

    struct SlistGuard {
        curl_slist* value = nullptr;

        ~SlistGuard()
        {
            if (value != nullptr) {
                curl_slist_free_all(value);
            }
        }
    };

    CURL* reuse_handle()
    {
        static thread_local CurlHandle handle;
        return handle.value;
    }

    curl_slist* build_header_list(const std::vector<std::string>& headers)
    {
        curl_slist* list = nullptr;
        for (const auto& h : headers) {
            list = curl_slist_append(list, h.c_str());
        }
        return list;
    }

    Status perform(const std::string& url,
        const std::vector<std::string>& headers, const std::string& payload,
        bool post, long timeout_secs, long max_redirs, std::string& body,
        std::size_t max_bytes, long* http_code, bool* truncated,
        std::vector<std::string>* response_headers = nullptr, bool del = false)
    {
        CURL* handle = reuse_handle();
        if (!handle) {
            return Status::NETWORK_ERROR;
        }
        const SlistGuard list { build_header_list(headers) };

        BodySink sink { &body, max_bytes, false };
        curl_easy_reset(handle);
        curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, timeout_secs);
        curl_easy_setopt(handle, CURLOPT_TIMEOUT, timeout_secs);
        curl_easy_setopt(
            handle, CURLOPT_FOLLOWLOCATION, max_redirs > 0 ? 1L : 0L);
        if (max_redirs > 0) {
            curl_easy_setopt(handle, CURLOPT_MAXREDIRS, max_redirs);
        }
        curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, list.value);
        if (post) {
            curl_easy_setopt(handle, CURLOPT_POST, 1L);
            curl_easy_setopt(handle, CURLOPT_POSTFIELDS, payload.c_str());
            curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE,
                static_cast<long>(payload.size()));
        } else if (del) {
            curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "DELETE");
        } else {
            curl_easy_setopt(handle, CURLOPT_HTTPGET, 1L);
        }
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, append_body);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &sink);
        if (response_headers != nullptr) {
            curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, capture_headers);
            curl_easy_setopt(handle, CURLOPT_HEADERDATA, response_headers);
        }

        const CURLcode res = curl_easy_perform(handle);

        long code = 0;
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &code);
        if (http_code != nullptr) {
            *http_code = code;
        }
        if (res != CURLE_OK) {
            if (res == CURLE_WRITE_ERROR && sink.hit_cap) {
                if (truncated != nullptr) {
                    *truncated = true;
                }
                return Status::OK;
            }
            return Status::NETWORK_ERROR;
        }
        return Status::OK;
    }

} // namespace

Status http_get(const std::string& url, const std::vector<std::string>& headers,
    long timeout_secs, std::string& body, long* http_code,
    const HttpGetOptions& opts)
{
    return perform(url, headers, { }, false, timeout_secs, opts.max_redirs,
        body, opts.max_bytes, http_code, opts.truncated);
}

Status http_post(const std::string& url,
    const std::vector<std::string>& headers, const std::string& payload,
    long timeout_secs, std::string& body, long* http_code,
    const HttpPostOptions& opts)
{
    return perform(url, headers, payload, true, timeout_secs, opts.max_redirs,
        body, 0, http_code, nullptr, opts.response_headers);
}

Status http_delete(const std::string& url,
    const std::vector<std::string>& headers, long timeout_secs, long* http_code)
{
    std::string body;
    return perform(url, headers, { }, false, timeout_secs, 0, body, 0,
        http_code, nullptr, nullptr, true);
}

namespace {

    // Strips trailing CRLF and parses "HTTP/x.y CODE" into `status`.
    template <typename T>
    void parse_status_line(std::string_view line, T& status)
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.remove_suffix(1);
        }
        if (line.starts_with("HTTP/")) {
            const auto sp = line.find(' ');
            if (sp != std::string_view::npos) {
                std::string_view v = line.substr(sp + 1);
                std::from_chars(v.data(), v.data() + v.size(), status);
            }
        }
    }

    // Extracts complete lines (LF or CRLF) from `buf` and passes each to
    // `consume`; returns the byte count consumed so the caller can erase.
    // Stops early when `consume` returns false.
    template <typename Consume>
    std::size_t consume_sse_lines(const std::string& buf, Consume&& consume)
    {
        std::size_t pos = 0;
        while (true) {
            const std::size_t nl = buf.find('\n', pos);
            if (nl == std::string::npos) {
                break;
            }
            std::string_view line(buf.data() + pos, nl - pos);
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            if (!consume(line)) {
                return nl + 1;
            }
            pos = nl + 1;
        }
        return pos;
    }

    // For "field: value" SSE lines: the value with one optional leading
    // space stripped; nullopt when the line is a different field.
    std::optional<std::string_view> sse_field_value(
        std::string_view line, std::string_view prefix)
    {
        if (!line.starts_with(prefix)) {
            return std::nullopt;
        }
        std::string_view v = line.substr(prefix.size());
        if (!v.empty() && v.front() == ' ') {
            v = v.substr(1);
        }
        return v;
    }

    // Incremental SSE parser for the notification stream: event/data/id/
    // retry fields accumulate until a blank line dispatches the event.
    struct SseStreamCtx {
        const std::atomic_bool* stop     = nullptr;
        const SseEventCallback* on_event = nullptr;
        long http_status                 = 0;
        std::string buf;
        std::string event;
        std::string data;
        std::string id;
        long retry_ms  = 0;
        bool has_retry = false;
    };

    size_t sse_header_callback(
        char* ptr, size_t size, size_t nmemb, void* userdata)
    {
        auto* ctx          = static_cast<SseStreamCtx*>(userdata);
        const size_t total = size * nmemb;
        parse_status_line(std::string_view(ptr, total), ctx->http_status);
        return total;
    }

    void sse_dispatch(SseStreamCtx& ctx)
    {
        if (ctx.event.empty() && ctx.data.empty()) {
            return;
        }
        const SseEvent event { ctx.event.empty() ? std::string_view("message")
                                                 : std::string_view(ctx.event),
            ctx.data, ctx.id, ctx.retry_ms, ctx.has_retry };
        if (ctx.on_event != nullptr) {
            (*ctx.on_event)(event);
        }
        ctx.event.clear();
        ctx.data.clear();
        ctx.id.clear();
        ctx.retry_ms  = 0;
        ctx.has_retry = false;
    }

    void sse_process_line(SseStreamCtx& ctx, std::string_view line)
    {
        if (line.empty()) {
            sse_dispatch(ctx);
            return;
        }
        if (const auto d = sse_field_value(line, "data:")) {
            if (!ctx.data.empty()) {
                ctx.data += "\n";
            }
            ctx.data.append(*d);
        } else if (const auto e = sse_field_value(line, "event:")) {
            ctx.event.assign(*e);
        } else if (const auto i = sse_field_value(line, "id:")) {
            ctx.id.assign(*i);
        } else if (const auto r = sse_field_value(line, "retry:")) {
            long value = 0;
            if (std::from_chars(r->data(), r->data() + r->size(), value).ec
                == std::errc { }) {
                ctx.retry_ms  = value;
                ctx.has_retry = true;
            }
        }
        // Comment (":...") and unknown fields are ignored per the SSE spec.
    }

    size_t sse_write_callback(char* ptr, size_t, size_t n, void* userdata)
    {
        auto* ctx = static_cast<SseStreamCtx*>(userdata);
        ctx->buf.append(ptr, n);
        ctx->buf.erase(
            0, consume_sse_lines(ctx->buf, [ctx](std::string_view line) {
                sse_process_line(*ctx, line);
                return true;
            }));
        return n;
    }

    int sse_progress_callback(
        void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
    {
        const auto* ctx = static_cast<const SseStreamCtx*>(userdata);
        return ctx->stop != nullptr && ctx->stop->load() ? 1 : 0;
    }

} // namespace

Status http_sse_get(const std::string& url,
    const std::vector<std::string>& headers, const std::atomic_bool& stop,
    const SseEventCallback& on_event, const std::string& last_event_id,
    long* http_code)
{
    // Dedicated handle: event callbacks may run nested HTTP calls (e.g.
    // answering a server ping) and must not re-enter reuse_handle().
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        return Status::NETWORK_ERROR;
    }
    CurlHandle guard { curl };

    std::vector<std::string> header_strs = headers;
    if (!last_event_id.empty()) {
        header_strs.push_back("Last-Event-ID: " + last_event_id);
    }
    const SlistGuard list { build_header_list(header_strs) };
    SseStreamCtx ctx;
    ctx.stop     = &stop;
    ctx.on_event = &on_event;

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, CONNECT_TIMEOUT_SECS);
    // Streams sit idle between server messages; stall (5 min of silence)
    // instead of a hard total timeout.
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, STALL_LIMIT_BYTES_PER_S);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, STALL_WINDOW_SECS);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list.value);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sse_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, sse_header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, sse_progress_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);

    const CURLcode res = curl_easy_perform(curl);
    long code          = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (http_code != nullptr) {
        *http_code = code;
    }
    if (res == CURLE_ABORTED_BY_CALLBACK) {
        return Status::CANCELLED;
    }
    if (res != CURLE_OK) {
        // Stall aborts surface as CURLE_OPERATION_TIMEDOUT; the caller
        // treats both like a closed stream and reconnects.
        return res == CURLE_OPERATION_TIMEDOUT ? Status::TIMEOUT
                                               : Status::NETWORK_ERROR;
    }
    return Status::OK;
}

extern const Provider openai_provider;
extern const Provider openai_responses_provider;
extern const Provider anthropic_provider;

namespace {

    constexpr std::size_t RAW_CAP = 16 * 1024;

    struct StreamCtx {
        const Provider* provider = nullptr;
        StreamCallback cb;
        ParseState parse_state;
        std::string buf;
        std::string event;
        std::string data;
        std::string raw;
        std::vector<StreamEvent> outs;
        const ChatRequest* req = nullptr;
        int retry_after        = 0;
        int http_status        = 0;
        bool connected         = false;
    };

    void mark_connected(StreamCtx& ctx)
    {
        if (ctx.connected || !http_ok(ctx.http_status)) {
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
        parse_status_line(line, ctx->http_status);
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
        if (const auto d = sse_field_value(line, "data:")) {
            if (!ctx.data.empty()) {
                ctx.data += "\n";
            }
            ctx.data.append(*d);
        } else if (const auto e = sse_field_value(line, "event:")) {
            ctx.event.assign(*e);
        }
    }

    size_t write_callback(char* ptr, size_t, size_t n, void* userdata)
    {
        auto* ctx = static_cast<StreamCtx*>(userdata);
        if (ctx->parse_state.terminal) {
            return n;
        }
        if (ctx->raw.size() < RAW_CAP) {
            ctx->raw.append(ptr, std::min(n, RAW_CAP - ctx->raw.size()));
        }
        mark_connected(*ctx);
        ctx->buf.append(ptr, n);
        ctx->buf.erase(
            0, consume_sse_lines(ctx->buf, [ctx](std::string_view line) {
                process_line(*ctx, line);
                return !ctx->parse_state.terminal;
            }));
        return n;
    }

    int progress_callback(
        void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
    {
        const auto* ctx = static_cast<const StreamCtx*>(userdata);
        return (ctx->req && ctx->req->interrupted && ctx->req->interrupted())
                || ctx->parse_state.terminal
            ? 1
            : 0;
    }

} // namespace

Status classify_failure(long code, const std::string& raw, std::string& message)
{
    Status st = parse_api_error(raw, message);
    if (code == 429) {
        st = Status::RATE_LIMITED;
    } else if (code == 402) {
        st = Status::BUDGET_EXCEEDED;
    } else if (code == 408 || code == 409 || code >= 500) {
        // Transient server-side failures; the turn runner may retry them
        // before any stream data arrives.
        st = Status::SERVER_ERROR;
    } else if (st == Status::OK) {
        st = Status::API_ERROR;
    }
    if (message.empty()) {
        message = "HTTP " + std::to_string(code);
    }
    return st;
}

Status stream(const Route& route, const ChatRequest& req, StreamCallback cb,
    int* retry_after)
{
    if (route.error != Status::OK) {
        cb(make_error_event(route.error, route.error_message));
        return route.error;
    }
    const Provider& provider = get_provider(route);
    const std::string body   = provider.build(req);
    const std::string& url   = route.endpoint;

    std::vector<std::string> header_strs
        = request_headers(route, stream_headers());
    if (!route.opencode_session.empty()) {
        header_strs.push_back("x-opencode-session: " + route.opencode_session);
    }
    const SlistGuard list { build_header_list(header_strs) };

    StreamCtx ctx;
    ctx.provider = &provider;
    ctx.cb       = std::move(cb);
    ctx.req      = &req;
    CURL* curl   = reuse_handle();
    if (!curl) {
        return Status::NETWORK_ERROR;
    }

    char errbuf[CURL_ERROR_SIZE] = { };
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, CONNECT_TIMEOUT_SECS);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, STALL_LIMIT_BYTES_PER_S);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, STALL_WINDOW_SECS);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list.value);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(
        curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);

    const CURLcode res = curl_easy_perform(curl);

    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

    if (res == CURLE_ABORTED_BY_CALLBACK && req.interrupted
        && req.interrupted()) {
        return Status::OK;
    }
    if (ctx.parse_state.terminal) {
        return Status::OK;
    }
    if (res != CURLE_OK) {
        std::string detail(
            errbuf[0] != '\0' ? errbuf : curl_easy_strerror(res));
        // LOW_SPEED aborts surface as CURLE_OPERATION_TIMEDOUT; retryable.
        const Status st = res == CURLE_OPERATION_TIMEDOUT
            ? Status::TIMEOUT
            : Status::NETWORK_ERROR;
        ctx.cb(make_error_event(st, std::move(detail)));
        return st;
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
    if (route.dialect == ApiStandard::OPENAI_RESPONSES) {
        return openai_responses_provider;
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

void emit_ready_tool_start(ToolAccum& acc, std::vector<StreamEvent>& outs)
{
    if (acc.started || acc.name.empty()) {
        return;
    }
    acc.started = true;
    outs.push_back(make_tool_call_start_event(finish_accum(acc)));
}

void emit_usage_once(
    ParseState& state, const Usage& usage, std::vector<StreamEvent>& outs)
{
    if (state.usage_emitted
        || (usage.prompt == 0 && usage.completion == 0 && usage.total == 0)) {
        return;
    }
    state.usage_emitted = true;
    outs.push_back(make_usage_event(usage));
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
    // The "error" member is polymorphic on the wire (object, string, or
    // absent), which reflection can't express in one struct; keep the
    // dynamic read for this genuinely variant payload.
    const JsonValue root = parse_json(body);
    if (!root.is_object()) {
        return Status::OK;
    }
    std::string msg;
    std::string kind;
    const JsonValue* err = find_member(root, "error");
    const auto text_of   = [](const JsonValue* v) -> std::string {
        return v != nullptr && v->is_string() ? v->as<std::string>() : "";
    };
    if (err != nullptr && err->is_object()) {
        msg  = text_of(find_member(*err, "message"));
        kind = text_of(find_member(*err, "type"));
        if (kind.empty()) {
            kind = text_of(find_member(*err, "code"));
        }
    } else if (err != nullptr && err->is_string()) {
        msg = err->as<std::string>();
    } else {
        msg = text_of(find_member(root, "message"));
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

} // namespace imza
