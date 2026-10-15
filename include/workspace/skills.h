#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace imza {

// One discovered skill: its identity, location, and where it was found.
// Shared by workspace discovery (which aggregates catalogs) and the
// tools domain (which loads and gates them).
struct Skill {
    enum class Scope { GLOBAL, PROJECT };
    std::string name;
    std::string description;
    std::filesystem::path path;
    Scope scope = Scope::GLOBAL;
    std::optional<std::filesystem::path> project_root;
};

} // namespace imza
