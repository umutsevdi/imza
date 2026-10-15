#include "permissions/filesystem.h"

#include <algorithm>
#include <system_error>
#include <type_traits>

#include "common/util.h"
#include "workspace/environment.h"

namespace imza {

namespace {

    template <typename T>
    constexpr bool is_read_request = std::is_same_v<T, ReadFileRequest>;

    template <typename T>
    constexpr bool is_list_request = std::is_same_v<T, ListDirectoryRequest>;

    template <typename T>
    constexpr bool is_find_request = std::is_same_v<T, FindFilesRequest>;

    constexpr const char* CANNOT_INSPECT_TARGET = "cannot inspect target";

    bool matches_path(const PermissionStore::Grants& grants,
        const std::filesystem::path& target)
    {
        return std::any_of(
            grants.begin(), grants.end(), [&](const auto& grant) {
                const auto* directory = std::get_if<ExternalGrant>(&grant);
                return directory != nullptr && path_within(*directory, target);
            });
    }

    FilesystemEvaluation reject(std::string reason)
    {
        return { { PermissionDecision::Kind::REJECT, std::move(reason) },
            std::nullopt };
    }

    bool is_read(const FilesystemRequest& request)
    {
        return std::holds_alternative<ReadFileRequest>(request);
    }

    bool is_list(const FilesystemRequest& request)
    {
        return std::holds_alternative<ListDirectoryRequest>(request);
    }

    bool is_find(const FilesystemRequest& request)
    {
        return std::holds_alternative<FindFilesRequest>(request);
    }

    bool is_edit(const FilesystemRequest& request)
    {
        return std::holds_alternative<InsertFileRequest>(request)
            || std::holds_alternative<EditFileRequest>(request);
    }

    bool is_write(const FilesystemRequest& request)
    {
        return std::holds_alternative<WriteFileRequest>(request);
    }

    bool is_mkdir(const FilesystemRequest& request)
    {
        return std::holds_alternative<MakeDirectoryRequest>(request);
    }

    bool is_remove(const FilesystemRequest& request)
    {
        return std::holds_alternative<RemoveRequest>(request);
    }

    void set_target(FilesystemRequest& request, std::filesystem::path target)
    {
        std::visit(
            [&](auto& value) { value.target = std::move(target); }, request);
    }

} // namespace

PermissionContext make_permission_context(const Environment& environment,
    const PermissionStore& permissions, SessionMode mode,
    std::vector<std::filesystem::path> skill_directories, const McpManager* mcp)
{
    return { environment.system(), environment.workspace(),
        permissions.snapshot(), mode, std::move(skill_directories), mcp };
}

const std::filesystem::path& filesystem_target(const FilesystemRequest& request)
{
    return std::visit(
        [](const auto& value) -> const std::filesystem::path& {
            return value.target;
        },
        request);
}

std::string_view filesystem_request_name(const FilesystemRequest& request)
{
    return std::visit(
        [](const auto& value) -> std::string_view {
            using T = std::decay_t<decltype(value)>;
            if constexpr (is_read_request<T>) {
                return "read";
            } else if constexpr (is_list_request<T>) {
                return "list";
            } else if constexpr (is_find_request<T>) {
                return "find";
            } else if constexpr (std::is_same_v<T, InsertFileRequest>) {
                return "insert";
            } else if constexpr (std::is_same_v<T, EditFileRequest>) {
                return "edit";
            } else if constexpr (std::is_same_v<T, MakeDirectoryRequest>) {
                return "mkdir";
            } else if constexpr (std::is_same_v<T, RemoveRequest>) {
                return "remove";
            } else {
                return "write";
            }
        },
        request);
}

FilesystemEvaluation evaluate_filesystem_request(
    FilesystemRequest request, const PermissionContext& context)
{
    if (!context.system || !context.workspace || !context.grants) {
        return reject("filesystem environment is not ready");
    }
    if (context.mode == SessionMode::PLAN
        && (is_edit(request) || is_write(request) || is_mkdir(request)
            || is_remove(request))) {
        return reject("unavailable in Plan mode");
    }

    std::filesystem::path target = filesystem_target(request);
    const std::filesystem::path& working_directory
        = context.workspace->working_directory;
    if (target.empty() || working_directory.empty()) {
        return reject("invalid path argument");
    }
    if (target.is_relative()) {
        target = working_directory / target;
    }
    std::error_code error;
    target = std::filesystem::weakly_canonical(target, error);
    if (error || !target.is_absolute()) {
        return reject("cannot normalize target");
    }
    const bool exists = std::filesystem::exists(target, error);
    if (error) {
        return reject(CANNOT_INSPECT_TARGET);
    }
    if ((is_read(request) || is_edit(request))
        && (!exists || !std::filesystem::is_regular_file(target, error))) {
        return reject("target is not a file");
    }
    if (is_list(request)
        && (!exists || !std::filesystem::is_directory(target, error))) {
        return reject("target is not a directory");
    }
    if (is_find(request)
        && (!exists
            || (!std::filesystem::is_directory(target, error)
                && !std::filesystem::is_regular_file(target, error)))) {
        return reject("target is not a file or directory");
    }
    if (is_write(request)) {
        if (exists && !std::filesystem::is_regular_file(target, error)) {
            return reject("target is not a file");
        }
        // save_text creates missing parent directories, so the check is
        // on the nearest existing ancestor: it must be a directory, else
        // the parents can never come into existence. A missing ancestor
        // is expected here (status reports not_found); only a stat that
        // cannot be interpreted is fatal.
        if (!exists) {
            std::filesystem::path parent = target.parent_path();
            bool parent_ok               = false;
            bool parent_error            = false;
            while (!parent.empty()) {
                std::error_code walk_error;
                const std::filesystem::file_status status
                    = std::filesystem::status(parent, walk_error);
                if (status.type() == std::filesystem::file_type::not_found) {
                    parent = parent.parent_path();
                    continue;
                }
                if (walk_error
                    || status.type() == std::filesystem::file_type::none) {
                    parent_error = true;
                    break;
                }
                parent_ok = std::filesystem::is_directory(status);
                break;
            }
            if (parent_error) {
                return reject(CANNOT_INSPECT_TARGET);
            }
            if (!parent_ok) {
                return reject("target parent is not a directory");
            }
        }
    }
    if (is_mkdir(request)) {
        // make_directories creates every missing parent; a non-directory
        // ancestor is reported by the effect itself.
        if (exists && !std::filesystem::is_directory(target, error)) {
            return reject("target is not a directory");
        }
    }
    if (is_remove(request)) {
        // The working directory, the project root, and any ancestor of
        // either are never removable: deleting them would destroy the
        // session's own ground. Checked first so it wins over the
        // emptiness rule for a non-empty root.
        const std::filesystem::path& root = context.workspace->project_root
            ? *context.workspace->project_root
            : working_directory;
        if (path_within(target, working_directory)
            || path_within(target, root)) {
            return reject("refusing to remove a session root directory");
        }
        if (!exists) {
            return reject("target does not exist");
        }
        const bool directory = std::filesystem::is_directory(target, error);
        if (error) {
            return reject(CANNOT_INSPECT_TARGET);
        }
        if (!directory && !std::filesystem::is_regular_file(target, error)) {
            return reject("target is not a file or directory");
        }
        if (directory && !std::filesystem::is_empty(target, error)) {
            if (error) {
                return reject(CANNOT_INSPECT_TARGET);
            }
            return reject("directory is not empty");
        }
    }
    if (error) {
        return reject(CANNOT_INSPECT_TARGET);
    }

    set_target(request, target);
    if (is_list(request) || is_find(request)) {
        return { { PermissionDecision::Kind::ACCEPT, "" }, request };
    }

    // Removal is its own class: it always asks, inside the workspace
    // included, unless a session grant already covers the target.
    if (is_remove(request)) {
        if (matches_path(*context.grants, target)) {
            return { { PermissionDecision::Kind::ACCEPT, "" }, request };
        }
        return { { PermissionDecision::Kind::ASK,
                     "removing files requires approval" },
            request };
    }

    // Approving a skill implies reading the files its instructions
    // reference; mutation there still needs its own approval.
    if (!is_edit(request) && !is_write(request) && !is_mkdir(request)
        && !is_remove(request)) {
        for (const std::filesystem::path& directory :
            context.skill_directories) {
            if (path_within(directory, target)) {
                return { { PermissionDecision::Kind::ACCEPT, "" }, request };
            }
        }
    }

    const bool write
        = is_edit(request) || is_write(request) || is_mkdir(request);
    const bool granted = matches_path(*context.grants, target);
    const std::filesystem::path& project_root = context.workspace->project_root
        ? *context.workspace->project_root
        : working_directory;
    const bool trusted = path_within(project_root, target)
        || path_within(context.system->temporary_directory, target);
    if (granted
        || (trusted && (!write || context.mode == SessionMode::BUILD))) {
        return { { PermissionDecision::Kind::ACCEPT, "" }, request };
    }
    return { { PermissionDecision::Kind::ASK,
                 "external directory access requires approval" },
        request };
}

std::optional<ExternalGrant> filesystem_session_grant(
    const FilesystemRequest& request)
{
    if (is_list(request) || is_find(request)) {
        return std::nullopt;
    }
    return filesystem_target(request).parent_path();
}

} // namespace imza
