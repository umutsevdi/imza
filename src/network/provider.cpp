#include <curl/curl.h>

#include <sstream>

#include "network.hpp"

namespace ursa {

extern const Provider openai_provider;
extern const Provider anthropic_provider;

namespace {

    struct StreamCtx {
        const Provider* provider;
        StreamCallback cb;
        std::string buf;
        std::string event;
        std::string data;
    };

    void dispatch_block(StreamCtx& ctx)
    {
        if (ctx.event.empty() && ctx.data.empty()) {
            return;
        }
        StreamEvent ev;
        ctx.provider->parse(ctx.event, ctx.data, ev);
        ctx.cb(ev);
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
            ctx.event = std::string(e);
        }
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

Status stream(const Provider& provider, const Config& cfg,
    const ChatRequest& req, StreamCallback cb)
{
    const std::string body = write_json(provider.build(req));
    const std::string url  = strip_slash(cfg.api_base) + provider.endpoint();

    const std::vector<std::string> header_strs = provider.headers(cfg.api_key);
    curl_slist* list                           = nullptr;
    for (const auto& h : header_strs) {
        list = curl_slist_append(list, h.c_str());
    }

    StreamCtx ctx { &provider, std::move(cb), { }, { }, { } };

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

Provider get_provider(const Config& cfg)
{
    if (cfg.standard == ApiStandard::OPENAI) {
        return openai_provider;
    }
    return anthropic_provider;
}

} // namespace ursa
