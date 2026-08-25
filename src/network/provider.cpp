#include <curl/curl.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <string_view>

#include "network.h"
#include "util.h"

namespace ursa {

extern const Provider openai_provider;
extern const Provider anthropic_provider;

namespace {

    constexpr std::size_t kRawCap = 16 * 1024;

    struct StreamCtx {
        const Provider* provider;
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
        auto* ctx         = static_cast<StreamCtx*>(userdata);
        const size_t total = size * nmemb;
        std::string_view line(ptr, total);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.remove_suffix(1);
        }
        if (line.starts_with("HTTP/")) {
            const auto sp = line.find(' ');
            if (sp != std::string_view::npos) {
                std::string_view v = line.substr(sp + 1);
                std::from_chars(v.data(), v.data() + v.size(),
                    ctx->http_status);
            }
        }
        constexpr std::string_view key = "retry-after:";
        if (line.size() > key.size()
            && std::equal(key.begin(), key.end(), line.begin(),
                [](char a, char b) {
                    return a
                        == std::tolower(static_cast<unsigned char>(b));
                })) {
            std::string_view v = line.substr(key.size());
            while (!v.empty() && v.front() == ' ') {
                v.remove_prefix(1);
            }
            ctx->retry_after = 0;
            std::from_chars(
                v.data(), v.data() + v.size(), ctx->retry_after);
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
        }
        ctx->buf.erase(0, pos);
        return n;
    }

    Status classify_failure(long code, const std::string& raw,
        std::string& message)
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

Status stream(const Provider& provider, const Config& cfg,
    const ChatRequest& req, StreamCallback cb, int* retry_after)
{
    const std::string body = write_json(provider.build(req));
    std::string url = strip_slash(cfg.api_base);
    url += provider.endpoint();

    const std::vector<std::string> header_strs = provider.headers(cfg.api_key);
    curl_slist* list                           = nullptr;
    for (const auto& h : header_strs) {
        list = curl_slist_append(list, h.c_str());
    }

    StreamCtx ctx;
    ctx.provider = &provider;
    ctx.cb       = std::move(cb);

    CURL* curl = reuse_handle();
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

    const CURLcode res = curl_easy_perform(curl);

    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(list);

    if (res != CURLE_OK) {
        std::string detail(errbuf[0] != '\0' ? errbuf : curl_easy_strerror(res));
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

Provider get_provider(const Config& cfg)
{
    if (cfg.standard == ApiStandard::OPENAI) {
        return openai_provider;
    }
    return anthropic_provider;
}

} // namespace ursa
