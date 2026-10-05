#pragma once

#include <optional>
#include <string>
#include <vector>

namespace imza {

// Typed tool arguments. Each tool parses its wire args into one of these
// with the shared Glaze options; validation errors map onto the stable
// model-facing tool_error messages at the call sites.

struct SkillToolArgs {
    std::string name;
    std::optional<std::string> scope;
    // The /skill flow and re-evaluated prompts bind an exact path.
    std::optional<std::string> path;
};

struct LoadToolArgs {
    std::string name;
};

struct SubagentTaskArgs {
    std::string mode;
    std::string prompt;
};

struct SubagentToolArgs {
    std::vector<SubagentTaskArgs> tasks;
};

struct LuaToolArgs {
    std::string script;
    std::optional<std::int64_t> timeout;
};

} // namespace imza