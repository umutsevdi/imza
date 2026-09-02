#include "agent/tools.h"
#include "common/util.h"
#include "network/json_io.h"
#include "network/web.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace ursa {

namespace {

    constexpr std::size_t MAX_OUTPUT_CHARS    = 40000;
    constexpr int DEFAULT_RESULTS             = 5;
    constexpr int MAX_RESULTS                 = 10;

    ToolOutput error(std::string text)
    {
        return { ToolOutput::Kind::ERROR, std::move(text) };
    }

    ToolOutput truncate_output(std::string text)
    {
        if (text.size() <= MAX_OUTPUT_CHARS) {
            return { ToolOutput::Kind::OUTPUT, std::move(text) };
        }
        std::string out(truncate_utf8(text, MAX_OUTPUT_CHARS));
        out += "\n[truncated: showing first "
            + std::to_string(out.size()) + " of the content]";
        return { ToolOutput::Kind::OUTPUT, std::move(out) };
    }

    bool looks_like_html(const std::string& body)
    {
        std::size_t begin = 0;
        while (begin < body.size()
            && std::isspace(static_cast<unsigned char>(body[begin]))) {
            ++begin;
        }
        if (begin >= body.size() || body[begin] != '<') {
            return false;
        }
        const std::string head
            = to_lower(body.substr(begin, std::min(body.size() - begin,
                std::size_t(200))));
        return head.starts_with("<!doctype html") || head.starts_with("<html")
            || head.starts_with("<head") || head.starts_with("<body")
            || head.starts_with("<div") || head.starts_with("<p")
            || head.starts_with("<h1") || head.starts_with("<h2")
            || head.starts_with("<!doctype html public");
    }

    ToolOutput webfetch_run(const Json::Value& args)
    {
        if (!args.isObject() || !args["url"].isString()
            || args["url"].asString().empty()) {
            return error("webfetch: 'url' must be a non-empty string");
        }
        const std::string url = args["url"].asString();

        FetchedPage page;
        std::string detail;
        const Status st = fetch_url(url, page, detail);
        if (st == Status::INVALID_URL) {
            return error("webfetch: " + detail + ": " + url);
        }
        if (st == Status::NETWORK_ERROR) {
            return error("webfetch: request failed: " + url);
        }
        if (st != Status::OK) {
            return error("webfetch: " + detail + ": " + url);
        }

        std::string text = looks_like_html(page.body)
            ? html_to_text(page.body)
            : page.body;
        if (trim(text).empty()) {
            return error("webfetch: no readable content at " + page.url);
        }
        return truncate_output(std::move(text));
    }

    ToolOutput websearch_run(const Json::Value& args)
    {
        if (!args.isObject() || !args["query"].isString()
            || args["query"].asString().empty()) {
            return error("websearch: 'query' must be a non-empty string");
        }
        const std::string query = args["query"].asString();

        int num_results = DEFAULT_RESULTS;
        if (args["num_results"].isInt()) {
            num_results = std::clamp(
                args["num_results"].asInt(), 1, MAX_RESULTS);
        }

        std::string text;
        const Status st = web_search(query, num_results, text);
        if (st == Status::NETWORK_ERROR) {
            return error("websearch: request failed for '" + query + "'");
        }
        if (st != Status::OK) {
            return error("websearch: search request rejected for '" + query
                + "'");
        }
        if (trim(text).empty()) {
            return { ToolOutput::Kind::OUTPUT,
                "No search results found. Try a different query." };
        }
        return truncate_output(std::move(text));
    }

} // namespace

Tool make_webfetch_tool()
{
    ToolSpec spec;
    spec.name = "webfetch";
    spec.description
        = "Fetch a web page over HTTP(S) and return its textual content. "
          "HTML pages are reduced to plain text (scripts, styles and markup "
          "are removed); other text resources are returned as-is. Long "
          "output is truncated.";
    spec.parameters = parse_json(
        R"json({"type":"object","properties":{"url":{"type":"string","description":"fully-qualified URL to fetch (http:// is upgraded to https://)"}},"required":["url"]})json");
    return { std::move(spec), webfetch_run, ToolSafety::READ_ONLY };
}

Tool make_websearch_tool()
{
    ToolSpec spec;
    spec.name = "websearch";
    spec.description
        = "Search the web and return a formatted list of results with "
          "titles, URLs and snippets. Use for information beyond your "
          "knowledge cutoff, current events, or to locate documentation. "
          "Follow up with webfetch to read a specific result.";
    spec.parameters = parse_json(
        R"json({"type":"object","properties":{"query":{"type":"string","description":"search query"},"num_results":{"type":"integer","description":"number of results to return (default 5, max 10)"}},"required":["query"]})json");
    return { std::move(spec), websearch_run, ToolSafety::READ_ONLY };
}

} // namespace ursa
