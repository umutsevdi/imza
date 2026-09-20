#include "tools/bindings.h"

#include "common/util.h"
#include "network/web.h"

#include <cctype>
#include <string>
#include <utility>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace imza {
namespace {

    constexpr std::size_t MAX_WEB_CHARS = 40000;

    // Cap at a UTF-8 boundary, then mark.
    std::string truncate_web(std::string text)
    {
        if (text.size() <= MAX_WEB_CHARS) {
            return text;
        }
        std::string out(truncate_utf8(text, MAX_WEB_CHARS));
        out += "\n[truncated: showing first " + std::to_string(out.size())
            + " of the content]";
        return out;
    }

    bool web_enabled(lua_State* L)
    {
        LuaRunContext* run = run_of(L);
        return run->host != nullptr && run->host->web_enabled;
    }

    int tool_webfetch(lua_State* L)
    {
        const std::string url = luaL_checkstring(L, 1);
        if (!web_enabled(L)) {
            return binding_error(
                L, "web.fetch: web access is disabled for this run");
        }

        FetchedPage page;
        std::string detail;
        const Status st = fetch_url(url, page, detail);
        if (st == Status::INVALID_URL) {
            record_call(L, "web.fetch", url, false);
            return binding_error(L, "web.fetch: " + detail + ": " + url);
        }
        if (st == Status::NETWORK_ERROR) {
            record_call(L, "web.fetch", url, false);
            return binding_error(L, "web.fetch: request failed: " + url);
        }
        if (st != Status::OK) {
            record_call(L, "web.fetch", url, false);
            return binding_error(L, "web.fetch: " + detail + ": " + url);
        }
        record_call(L, "web.fetch", url, true);

        std::string body  = page.body;
        std::size_t begin = 0;
        while (begin < body.size()
            && std::isspace(static_cast<unsigned char>(body[begin]))) {
            ++begin;
        }
        const std::string head = to_lower(body.substr(
            begin, std::min(body.size() - begin, std::size_t(200))));
        const bool is_html     = !body.empty() && body[begin] == '<'
            && (head.starts_with("<!doctype html") || head.starts_with("<html")
                || head.starts_with("<head") || head.starts_with("<body")
                || head.starts_with("<div") || head.starts_with("<p")
                || head.starts_with("<h1") || head.starts_with("<h2")
                || head.starts_with("<!doctype html public"));
        std::string text = is_html ? html_to_text(page.body) : page.body;
        if (trim(text).empty()) {
            return binding_error(
                L, "web.fetch: no readable content at " + page.url);
        }
        const std::string out = truncate_web(std::move(text));
        lua_pushlstring(L, out.data(), out.size());
        return 1;
    }

    int tool_websearch(lua_State* L)
    {
        const std::string query = luaL_checkstring(L, 1);
        int num_results         = 5;
        if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
            num_results = static_cast<int>(luaL_checkinteger(L, 2));
        }
        num_results = std::clamp(num_results, 1, 10);
        if (!web_enabled(L)) {
            return binding_error(
                L, "web.search: web access is disabled for this run");
        }

        std::string text;
        const Status st = web_search(query, num_results, text);
        if (st == Status::NETWORK_ERROR) {
            record_call(L, "web.search", query, false);
            return binding_error(
                L, "web.search: request failed for '" + query + "'");
        }
        if (st != Status::OK) {
            record_call(L, "web.search", query, false);
            return binding_error(
                L, "web.search: search request rejected for '" + query + "'");
        }
        record_call(L, "web.search", query, true);
        if (trim(text).empty()) {
            lua_pushliteral(
                L, "No search results found. Try a different query.");
            return 1;
        }
        const std::string out = truncate_web(std::move(text));
        lua_pushlstring(L, out.data(), out.size());
        return 1;
    }

    constexpr LuaBinding BINDINGS[] = {
        {
            "web.fetch",
            tool_webfetch,
            "tool.web.fetch(url: string) => string",
            "Fetches an http(s) URL as readable text: HTML is converted to "
            "plain\n"
            "text, other bodies (JSON, markdown, raw) return as-is.\n"
            "Fails on non-http(s) URLs, network errors, non-2xx responses, "
            "bodies over 5 MB,\n"
            "and pages with no readable content. Capped at 40000 characters.",
            LuaCapability::WEB,
        },
        {
            "web.search",
            tool_websearch,
            "tool.web.search(query: string, num_results?: integer=5) => string",
            "Search results as a formatted text block.\n"
            "num_results is clamped to 1..10. No hits returns \"No search "
            "results found.\n"
            "Try a different query.\"",
            LuaCapability::WEB,
        },
    };

} // namespace

std::span<const LuaBinding> web_lua_bindings() { return BINDINGS; }

} // namespace imza
