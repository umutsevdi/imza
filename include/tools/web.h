#pragma once

#include "common/types.h"

#include <string>

namespace imza {

// One web search through the Exa MCP endpoint (https://mcp.exa.ai/mcp).
// EXA_API_KEY is picked up from the environment when present; `text`
// receives the first text block of the web_search_exa tool result.
Status web_search(const std::string& query, int num_results, std::string& text);

} // namespace imza
