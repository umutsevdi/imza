#pragma once

#include <string>
#include <variant>

#include "permissions/filesystem.h"
#include "permissions/shell.h"

namespace imza {

// The skill a prompt is about, rendered as the modal's details.
struct SkillRequest {
    std::string name;
    std::string scope;
    std::string path;

    bool operator==(const SkillRequest&) const = default;
};

// The gated call's own parameters, typed per family.
using PermissionPromptRequest = std::variant<std::monostate, FilesystemRequest,
    ShellRequest, SkillRequest>;

// Approval modal payload for a gated call. `id` is the originating
// tool-call id, or "manual-skill" for the /skill flow.
struct PermissionPrompt {
    std::string name;
    std::string description;
    std::string reason;
    std::string target;
    bool allow_for_session = false;
    std::string id;
    PermissionPromptRequest request;
};

} // namespace imza
