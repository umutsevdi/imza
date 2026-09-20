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

    template <typename T>
    constexpr bool is_edit_request = std::is_same_v<T, InsertFileRequest>
        || std::is_same_v<T, EditFileRequest>;

    template <typename T>
    constexpr bool is_write_request = std::is_same_v<T, WriteFileRequest>;

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

    void set_target(FilesystemRequest& request, std::filesystem::path target)
    {
        std::visit(
            [&](auto& value) { value.target = std::move(target); }, request);
    }

} // namespace

PermissionContext permission_context(const Environment& environment,
    const PermissionStore& permissions, Session::Mode mode)
{
    return { environment.system(), environment.workspace(),
        permissions.snapshot(), mode };
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
    const std::string name(filesystem_request_name(request));
    if (context.mode == Session::Mode::PLAN
        && (is_edit(request) || is_write(request))) {
        return reject(name + ": unavailable in Plan mode");
    }

    std::filesystem::path target = filesystem_target(request);
    const std::filesystem::path& working_directory
        = context.workspace->working_directory;
    if (target.empty() || working_directory.empty()) {
        return reject(name + ": invalid path argument");
    }
    if (target.is_relative()) {
        target = working_directory / target;
    }
    std::error_code error;
    target = std::filesystem::weakly_canonical(target, error);
    if (error || !target.is_absolute()) {
        return reject(name + ": cannot normalize target");
    }
    const bool exists = std::filesystem::exists(target, error);
    if (error) {
        return reject(name + ": cannot inspect target");
    }
    if ((is_read(request) || is_edit(request))
        && (!exists || !std::filesystem::is_regular_file(target, error))) {
        return reject(name + ": target is not a file");
    }
    if (is_list(request)
        && (!exists || !std::filesystem::is_directory(target, error))) {
        return reject("list: target is not a directory");
    }
    if (is_find(request)
        && (!exists
            || (!std::filesystem::is_directory(target, error)
                && !std::filesystem::is_regular_file(target, error)))) {
        return reject("find: target is not a file or directory");
    }
    if (is_write(request)) {
        if (exists && !std::filesystem::is_regular_file(target, error)) {
            return reject("write: target is not a file");
        }
        if (!exists
            && !std::filesystem::is_directory(target.parent_path(), error)) {
            return reject("write: target parent is not a directory");
        }
    }
    if (error) {
        return reject(name + ": cannot inspect target");
    }

    set_target(request, target);
    if (is_list(request) || is_find(request)) {
        return { { PermissionDecision::Kind::ACCEPT, "" }, request };
    }

    const bool write   = is_edit(request) || is_write(request);
    const bool granted = matches_path(*context.grants, target);
    const std::filesystem::path& project_root = context.workspace->project_root
        ? *context.workspace->project_root
        : working_directory;
    const bool trusted = path_within(project_root, target)
        || path_within(context.system->temporary_directory, target);
    if (granted
        || (trusted && (!write || context.mode == Session::Mode::BUILD))) {
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
