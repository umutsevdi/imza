#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <variant>

namespace imza {

// Typed request values for the permission gate. These are the shared
// vocabulary between the callers that construct requests, the evaluator
// in permissions/ that canonicalizes and authorizes them, and the modal
// payloads that present them; policy itself stays in the permissions
// domain.

struct ReadFileRequest {
    std::filesystem::path target;
    std::size_t first_line = 1;
    std::optional<std::size_t> last_line;

    bool operator==(const ReadFileRequest&) const = default;
};

struct ListDirectoryRequest {
    std::filesystem::path target = ".";
    int depth                    = 1;
    bool show_hidden             = false;

    bool operator==(const ListDirectoryRequest&) const = default;
};

struct FindFilesRequest {
    std::filesystem::path target = ".";
    std::string pattern;

    bool operator==(const FindFilesRequest&) const = default;
};

struct InsertFileRequest {
    std::filesystem::path target;
    std::string text;
    std::optional<std::size_t> line;

    bool operator==(const InsertFileRequest&) const = default;
};

struct EditFileRequest {
    std::filesystem::path target;
    std::string old_text;
    std::string new_text;
    std::size_t count = 1;

    bool operator==(const EditFileRequest&) const = default;
};

struct WriteFileRequest {
    std::filesystem::path target;
    std::string text;

    bool operator==(const WriteFileRequest&) const = default;
};

// Creates `target` and every missing parent. Write-class: trusted roots
// accept it in Build, external targets ask like any other write.
struct MakeDirectoryRequest {
    std::filesystem::path target;

    bool operator==(const MakeDirectoryRequest&) const = default;
};

// Removes a file, or a directory only when it is empty. Its own policy
// class: it always asks, inside the workspace included.
struct RemoveRequest {
    std::filesystem::path target;

    bool operator==(const RemoveRequest&) const = default;
};

// Copies or moves a regular file to a new path. Write-class on the
// destination; the source is only read. `target` is the destination so
// the shared path checks and the session grant apply where the effect
// lands.
struct CopyFileRequest {
    std::filesystem::path source;
    std::filesystem::path target;

    bool operator==(const CopyFileRequest&) const = default;
};

using FilesystemRequest = std::variant<ReadFileRequest, ListDirectoryRequest,
    FindFilesRequest, InsertFileRequest, EditFileRequest, WriteFileRequest,
    MakeDirectoryRequest, RemoveRequest, CopyFileRequest>;

struct ShellRequest {
    std::string command;
    std::chrono::seconds timeout { 10 };
    std::filesystem::path workspace;
    // Merged over the inherited environment for the child process; rides
    // the request so execution uses the authorized parameters.
    std::map<std::string, std::string> env;

    bool operator==(const ShellRequest&) const = default;
};

// A shell command the config allowlist grants outright; the evaluator
// matches executed commands against granted program/subcommand pairs.
struct ShellCommandGrant {
    std::string program;
    std::optional<std::string> subcommand;

    bool operator==(const ShellCommandGrant&) const = default;
};

// The skill a prompt is about, rendered as the modal's details.
struct SkillRequest {
    std::string name;
    std::string scope;
    std::string path;

    bool operator==(const SkillRequest&) const = default;
};

// The MCP server a load_mcp prompt is about: the id, its display label,
// and the manager's own failure/detail text when present.
struct McpRequest {
    std::string id;
    std::string label;
    std::string detail;

    bool operator==(const McpRequest&) const = default;
};

// The gated call's own parameters, typed per family.
using PermissionPromptRequest = std::variant<std::monostate, FilesystemRequest,
    ShellRequest, SkillRequest, McpRequest>;

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
