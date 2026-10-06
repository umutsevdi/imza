#include "tools/mcp_catalog.h"

#include "mcp_catalog.inc"
#include "network/json.h"

#include <glaze/glaze.hpp>

namespace imza {

namespace {

    // Wire-shaped mirror of the bundled catalogue document; unknown
    // members are tolerated so entries can grow metadata.
    struct StoredCatalogEntry {
        std::optional<std::string> id;
        std::optional<std::string> label;
        std::optional<std::string> url;
        std::optional<std::string> description;
        std::optional<std::string> auth_kind;
        std::optional<std::string> auth_hint;
    };

    struct StoredCatalog {
        std::vector<StoredCatalogEntry> servers;
    };

} // namespace

std::vector<McpCatalogEntry> load_mcp_catalog()
{
    StoredCatalog stored;
    if (json_parse_checked(EMBEDDED_MCP_CATALOG, stored)) {
        return { };
    }
    std::vector<McpCatalogEntry> out;
    out.reserve(stored.servers.size());
    for (auto& entry : stored.servers) {
        McpCatalogEntry parsed;
        parsed.id          = entry.id.value_or("");
        parsed.label       = entry.label.value_or("");
        parsed.url         = entry.url.value_or("");
        parsed.description = entry.description.value_or("");
        parsed.auth_kind   = entry.auth_kind.value_or("none");
        parsed.auth_hint   = entry.auth_hint.value_or("");
        if (parsed.id.empty() || parsed.url.empty()) {
            continue;
        }
        out.push_back(std::move(parsed));
    }
    return out;
}

std::optional<McpCatalogEntry> find_mcp_catalog_entry(std::string_view id)
{
    for (const McpCatalogEntry& entry : load_mcp_catalog()) {
        if (entry.id == id) {
            return entry;
        }
    }
    return std::nullopt;
}

} // namespace imza
