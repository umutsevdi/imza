#include "tools/mcp_catalog.h"

#include "mcp_catalog.inc"
#include "network/json.h"

#include <glaze/glaze.hpp>

namespace imza {

namespace {

    // Wire-shaped mirror of the bundled catalogue document; unknown
    // members are tolerated so entries can grow metadata.
} // namespace

// Glaze-reflected: must have external linkage (Clang/MSVC requirement).
struct StoredCatalogEntry {
    std::optional<std::string> id;
    std::optional<std::string> label;
    std::optional<std::string> url;
    std::optional<std::string> auth_kind;
};

struct StoredCatalog {
    std::vector<StoredCatalogEntry> servers;
};

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
        parsed.id        = entry.id.value_or("");
        parsed.label     = entry.label.value_or("");
        parsed.url       = entry.url.value_or("");
        parsed.auth_kind = entry.auth_kind.value_or("none");
        if (parsed.id.empty() || parsed.url.empty()) {
            continue;
        }
        out.push_back(std::move(parsed));
    }
    return out;
}

std::optional<McpCatalogEntry> find_mcp_catalog_entry(std::string_view id)
{
    // The catalogue is a compile-time constant; parse it once.
    static const std::vector<McpCatalogEntry> catalog = load_mcp_catalog();
    for (const McpCatalogEntry& entry : catalog) {
        if (entry.id == id) {
            return entry;
        }
    }
    return std::nullopt;
}

std::string resolved_mcp_url(const McpServerConfig& config)
{
    if (!config.url.empty() || config.catalog_id.empty()) {
        return expand_env_vars(config.url);
    }
    const auto entry = find_mcp_catalog_entry(config.catalog_id);
    return entry ? expand_env_vars(entry->url) : std::string { };
}

} // namespace imza
