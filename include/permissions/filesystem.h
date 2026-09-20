#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "common/types.h"
#include "permissions/store.h"

namespace imza {

class Environment;
struct SystemEnvironment;
struct WorkspaceEnvironment;

struct PermissionContext {
    std::shared_ptr<const SystemEnvironment> system;
    std::shared_ptr<const WorkspaceEnvironment> workspace;
    PermissionStore::Snapshot grants;
    SessionMode mode = SessionMode::PLAN;
};

struct PermissionDecision {
    enum class Kind { ACCEPT, ASK, REJECT };

    Kind kind = Kind::REJECT;
    std::string reason;
};

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

using FilesystemRequest = std::variant<ReadFileRequest, ListDirectoryRequest,
    FindFilesRequest, InsertFileRequest, EditFileRequest, WriteFileRequest>;

struct FilesystemEvaluation {
    PermissionDecision decision;
    std::optional<FilesystemRequest> request;
};

PermissionContext make_permission_context(const Environment& environment,
    const PermissionStore& permissions, SessionMode mode);
FilesystemEvaluation evaluate_filesystem_request(
    FilesystemRequest request, const PermissionContext& context);
std::optional<ExternalGrant> filesystem_session_grant(
    const FilesystemRequest& request);
const std::filesystem::path& filesystem_target(
    const FilesystemRequest& request);
std::string_view filesystem_request_name(const FilesystemRequest& request);

} // namespace imza
