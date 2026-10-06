#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace imza {

// One curated entry of the bundled MCP server catalogue. The catalogue is
// the trust whitelist: it decides what /mcp presents as prebuilt and which
// auth guidance ships with it. Registry-derived data never gets this
// treatment.
struct McpCatalogEntry {
    std::string id;
    std::string label;
    std::string url;
    std::string description;
    std::string auth_kind; // "none" | "token" | "oauth"
    std::string auth_hint;
};

// Parses the embedded catalogue JSON; an unreadable file yields an empty
// catalogue, never a startup failure.
std::vector<McpCatalogEntry> load_mcp_catalog();

// Catalogue entry by id; nullopt when the id is not in the catalogue
// (removed/renamed entries resolve to nothing).
std::optional<McpCatalogEntry> find_mcp_catalog_entry(std::string_view id);

} // namespace imza
