#include "network/web.h"

#include "common/util.h"
#include "network/json_io.h"
#include "network/network.h"

#include <json/json.h>

#include <cctype>
#include <cstdlib>
#include <map>
#include <string_view>
#include <vector>

namespace ursa {

namespace {

    constexpr long FETCH_TIMEOUT_SECS    = 30;
    constexpr long SEARCH_TIMEOUT_SECS   = 25;
    constexpr std::size_t MAX_BODY_BYTES = 5 * 1024 * 1024;

#if defined(_WIN32)
    constexpr std::string_view UA_PLATFORM = "Windows NT 10.0; Win64; x64";
#elif defined(__APPLE__)
    constexpr std::string_view UA_PLATFORM = "Macintosh; Intel Mac OS X 10.15";
#else
    constexpr std::string_view UA_PLATFORM = "X11; Linux x86_64";
#endif

    const std::string& user_agent()
    {
        static const std::string ua = "User-Agent: Mozilla/5.0 ("
            + std::string(UA_PLATFORM)
            + "; rv:128.0) Gecko/20100101 Firefox/128.0";
        return ua;
    }

    bool is_ignored_tag(std::string_view name)
    {
        return name == "script" || name == "style" || name == "noscript"
            || name == "template" || name == "head" || name == "iframe"
            || name == "object" || name == "embed" || name == "svg";
    }

    bool is_block_tag(std::string_view name)
    {
        return name == "p" || name == "div" || name == "br" || name == "li"
            || name == "ul" || name == "ol" || name == "tr" || name == "table"
            || name == "thead" || name == "tbody" || name == "h1"
            || name == "h2" || name == "h3" || name == "h4" || name == "h5"
            || name == "h6" || name == "blockquote" || name == "pre"
            || name == "section" || name == "article" || name == "header"
            || name == "footer" || name == "nav" || name == "aside"
            || name == "main" || name == "form" || name == "fieldset"
            || name == "dl" || name == "dt" || name == "dd" || name == "hr"
            || name == "figure" || name == "figcaption" || name == "address";
    }

    std::string percent_encode(std::string_view value)
    {
        static const char* hex = "0123456789ABCDEF";
        std::string out;
        out.reserve(value.size());
        for (const char c : value) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (std::isalnum(u) || c == '-' || c == '.' || c == '_'
                || c == '~') {
                out += c;
            } else {
                out += '%';
                out += hex[u >> 4];
                out += hex[u & 0xF];
            }
        }
        return out;
    }

    void append_utf8(std::string& out, unsigned int cp)
    {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    const std::map<std::string_view, std::string_view>& named_entities()
    {
        static const std::map<std::string_view, std::string_view> map = {
            { "amp", "&" }, { "lt", "<" }, { "gt", ">" }, { "quot", "\"" },
            { "apos", "'" }, { "nbsp", " " }, { "copy", "\u00A9" },
            { "reg", "\u00AE" }, { "trade", "\u2122" }, { "mdash", "\u2014" },
            { "ndash", "\u2013" }, { "hellip", "\u2026" },
            { "lsquo", "\u2018" }, { "rsquo", "\u2019" }, { "ldquo", "\u201C" },
            { "rdquo", "\u201D" }, { "middot", "\u00B7" }, { "bull", "\u2022" },
            { "laquo", "\u00AB" }, { "raquo", "\u00BB" }, { "deg", "\u00B0" },
            { "plusmn", "\u00B1" }, { "times", "\u00D7" },
            { "divide", "\u00F7" }, { "euro", "\u20AC" }, { "pound", "\u00A3" },
            { "yen", "\u00A5" }, { "cent", "\u00A2" }, { "sect", "\u00A7" },
            { "para", "\u00B6" }, { "szlig", "\u00DF" }, { "auml", "\u00E4" },
            { "ouml", "\u00F6" }, { "uuml", "\u00FC" }, { "Auml", "\u00C4" },
            { "Ouml", "\u00D6" }, { "Uuml", "\u00DC" }, { "sz", "\u00DF" }
        };
        return map;
    }

    // Decodes an entity spanning [begin+1, semi); appends to out and returns
    // true on success.
    bool decode_entity(const std::string& low, std::size_t begin,
        std::size_t semi, std::string& out)
    {
        const std::string_view body(low.data() + begin + 1, semi - begin - 1);
        if (body.empty()) {
            return false;
        }
        if (body.front() == '#') {
            const char* start = body.data() + 1;
            const char* end   = body.data() + body.size();
            int base          = 10;
            if (start < end && (*start == 'x' || *start == 'X')) {
                ++start;
                base = 16;
            }
            if (start >= end) {
                return false;
            }
            const unsigned long cp = std::strtoul(start, nullptr, base);
            if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
                return false;
            }
            append_utf8(out, static_cast<unsigned int>(cp));
            return true;
        }
        const auto& entities = named_entities();
        const auto it        = entities.find(body);
        if (it == entities.end()) {
            return false;
        }
        out += it->second;
        return true;
    }

    std::string collapse_blank_lines(std::string text)
    {
        std::string out;
        out.reserve(text.size());
        int run = 0;
        for (const char c : text) {
            if (c == '\n') {
                if (++run > 2) {
                    continue;
                }
            } else {
                run = 0;
            }
            out += c;
        }
        return out;
    }

    std::string text_from_json(const Json::Value& root)
    {
        static const Json::Value empty;
        if (!root.isObject()) {
            return "";
        }
        const Json::Value& result
            = root["result"].isObject() ? root["result"] : empty;
        const Json::Value& content = result["content"];
        if (!content.isArray()) {
            return "";
        }
        for (const auto& item : content) {
            if (!item.isObject() || !item["text"].isString()) {
                continue;
            }
            std::string text = item["text"].asString();
            if (!trim(text).empty()) {
                return text;
            }
        }
        return "";
    }

} // namespace

Status normalize_web_url(const std::string& raw, std::string& out)
{
    const std::string url(trim(raw));
    const std::string scheme
        = to_lower(url.size() >= 8 ? url.substr(0, 8) : url);
    if (scheme.starts_with("https://")) {
        out = url;
    } else if (scheme.starts_with("http://")) {
        out = "https://" + url.substr(7);
    } else {
        return Status::INVALID_URL;
    }
    return Status::OK;
}

Status fetch_url(
    const std::string& raw_url, FetchedPage& page, std::string& detail)
{
    detail.clear();
    std::string url;
    const Status normalized = normalize_web_url(raw_url, url);
    if (normalized != Status::OK) {
        detail = "only http(s) URLs are allowed";
        return normalized;
    }

    const std::vector<std::string> headers = {
        user_agent(),
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,"
        "*/*;q=0.8",
        "Accept-Language: en-US,en;q=0.9",
    };

    long code      = 0;
    bool truncated = false;
    std::string body;
    const HttpGetOptions opts { MAX_BODY_BYTES, &truncated };
    if (http_get(url, headers, FETCH_TIMEOUT_SECS, body, &code, opts)
        != Status::OK) {
        detail = "request failed";
        return Status::NETWORK_ERROR;
    }
    if (truncated) {
        detail = "response too large (limit 5 MB)";
        return Status::API_ERROR;
    }
    if (code < 200 || code >= 300) {
        page.http_code = code;
        detail         = "HTTP " + std::to_string(code);
        return Status::API_ERROR;
    }

    page.url       = std::move(url);
    page.body      = std::move(body);
    page.http_code = code;
    return Status::OK;
}

std::string html_to_text(const std::string& html)
{
    const std::string low = to_lower(html);
    std::string out;
    out.reserve(html.size() / 2 + 1);
    bool pending_space = false;

    const auto newline = [&] {
        pending_space = false;
        if (!out.empty() && out.back() != '\n') {
            out += '\n';
        }
    };
    const auto flush_space = [&] {
        if (pending_space) {
            if (!out.empty() && out.back() != '\n') {
                out += ' ';
            }
            pending_space = false;
        }
    };
    const auto add_char = [&](char c) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            pending_space = true;
            return;
        }
        flush_space();
        out += c;
    };

    std::size_t i = 0;
    while (i < html.size()) {
        if (low.compare(i, 4, "<!--") == 0) {
            const auto end = low.find("-->", i + 4);
            i              = end == std::string::npos ? html.size() : end + 3;
            continue;
        }
        if (html[i] == '<') {
            const auto gt = low.find('>', i);
            if (gt == std::string::npos) {
                break;
            }
            std::string_view tag(low.data() + i + 1, gt - i - 1);
            if (!tag.empty() && tag.front() == '/') {
                tag.remove_prefix(1);
            }
            const auto space            = tag.find_first_of(" \t\r\n/");
            const std::string_view name = tag.substr(
                0, space == std::string_view::npos ? tag.size() : space);
            if (is_ignored_tag(name)) {
                if (tag.empty() || tag.front() != '/') {
                    const std::string close = "</" + std::string(name);
                    const auto end          = low.find(close, gt);
                    if (end == std::string::npos) {
                        break;
                    }
                    const auto close_gt = low.find('>', end);
                    if (close_gt == std::string::npos) {
                        break;
                    }
                    newline();
                    i = close_gt + 1;
                    continue;
                }
                newline();
                i = gt + 1;
                continue;
            }
            if (is_block_tag(name)) {
                newline();
            }
            i             = gt + 1;
            pending_space = false;
            continue;
        }
        if (html[i] == '&') {
            const auto semi = low.find(';', i + 1);
            if (semi != std::string::npos && semi - i <= 10) {
                std::string decoded;
                if (decode_entity(low, i, semi, decoded)) {
                    flush_space();
                    out += decoded;
                    pending_space = false;
                    i             = semi + 1;
                    continue;
                }
            }
        }
        add_char(html[i]);
        ++i;
    }

    return std::string(trim(collapse_blank_lines(out)));
}

std::string mcp_search_text(const std::string& response)
{
    const Json::Value root = parse_json(response);
    if (!root.isNull()) {
        if (std::string text = text_from_json(root); !text.empty()) {
            return text;
        }
    }
    for (const auto& line : split_lines(response)) {
        std::string_view v = trim(line);
        if (!v.starts_with("data:")) {
            continue;
        }
        v.remove_prefix(5);
        if (!v.empty() && v.front() == ' ') {
            v.remove_prefix(1);
        }
        const Json::Value item = parse_json(v);
        if (item.isNull()) {
            continue;
        }
        if (std::string text = text_from_json(item); !text.empty()) {
            return text;
        }
    }
    return "";
}

Status web_search(const std::string& query, int num_results, std::string& text)
{
    std::string url = "https://mcp.exa.ai/mcp";
    if (const std::string key = env_or_empty("EXA_API_KEY"); !key.empty()) {
        url += "?exaApiKey=" + percent_encode(key);
    }

    Json::Value arguments;
    arguments["query"]      = query;
    arguments["numResults"] = num_results;
    arguments["type"]       = "auto";
    arguments["livecrawl"]  = "fallback";
    Json::Value params;
    params["name"]      = "web_search_exa";
    params["arguments"] = std::move(arguments);
    Json::Value body;
    body["jsonrpc"] = "2.0";
    body["id"]      = 1;
    body["method"]  = "tools/call";
    body["params"]  = std::move(params);

    long code = 0;
    std::string response;
    const std::vector<std::string> headers = {
        "Content-Type: application/json",
        "Accept: application/json, text/event-stream",
    };
    if (http_post(url, headers, write_json(body), SEARCH_TIMEOUT_SECS, response,
            &code, 0)
        != Status::OK) {
        return Status::NETWORK_ERROR;
    }
    if (code < 200 || code >= 300) {
        return Status::API_ERROR;
    }
    text = mcp_search_text(response);
    return Status::OK;
}

} // namespace ursa
