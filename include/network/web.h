#pragma once

#include "common/types.h"

#include <string>

namespace imza {

struct FetchedPage {
    std::string url;
    std::string body;
    long http_code = 0;
};

// Validates a raw URL (http/https only) and upgrades http:// to https://.
Status normalize_web_url(const std::string& raw, std::string& out);

Status fetch_url(
    const std::string& url, FetchedPage& page, std::string& detail);

std::string html_to_text(const std::string& html);

Status web_search(const std::string& query, int num_results, std::string& text);

} // namespace imza
