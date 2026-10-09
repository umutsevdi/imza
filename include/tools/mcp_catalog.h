#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "platform/config.h"

namespace imza {

// One curated entry of the bundled MCP server catalogue. The catalogue is
// the trust whitelist: it decides what /mcp presents as prebuilt and which
// auth guidance ships with it. Registry-derived data never gets this
// treatment.
struct McpCatalogEntry {
    std::string id;
    std::string label;
    std::string url;
    std::string auth_kind; // "none" | "token" | "oauth"
};

// Parses the embedded catalogue JSON; an unreadable file yields an empty
// catalogue, never a startup failure.
std::vector<McpCatalogEntry> load_mcp_catalog();

// Catalogue entry by id; nullopt when the id is not in the catalogue
// (removed/renamed entries resolve to nothing).
std::optional<McpCatalogEntry> find_mcp_catalog_entry(std::string_view id);

// Effective request URL for a server: the explicit url when set, otherwise
// the bundled catalogue entry's url for a catalog_id reference. Empty when
// neither resolves (a dangling reference), which callers surface as a
// connection failure rather than issuing a malformed request.
std::string resolved_mcp_url(const McpServerConfig& config);

} // namespace imza
