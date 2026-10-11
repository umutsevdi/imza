#include "tools/web.h"

#include "common/util.h"
#include "protocols/mcp.h"

#include <utility>

namespace imza {

namespace {

    constexpr long SEARCH_TIMEOUT_SECS = 25;

} // namespace

Status web_search(const std::string& query, int num_results, std::string& text)
{
    std::string url = "https://mcp.exa.ai/mcp";
    if (const std::string key = env_or_empty("EXA_API_KEY"); !key.empty()) {
        url += "?exaApiKey=" + percent_encode(key);
    }

    McpEndpoint endpoint;
    endpoint.url          = std::move(url);
    endpoint.timeout_secs = SEARCH_TIMEOUT_SECS;
    McpSession session;
    session.endpoint = std::move(endpoint);

    std::string detail;
    if (const Status st = mcp_initialize(session, detail); st != Status::OK) {
        return st;
    }

    JsonValue arguments;
    arguments["query"]      = query;
    arguments["numResults"] = static_cast<double>(num_results);
    arguments["type"]       = "auto";
    arguments["livecrawl"]  = "fallback";

    McpToolCallResult result;
    const Status st
        = mcp_call_tool(session, "web_search_exa", arguments, result, detail);
    mcp_end_session(session);
    if (st != Status::OK) {
        return st;
    }
    text = mcp_first_text(result);
    return Status::OK;
}

} // namespace imza
