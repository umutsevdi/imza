#include "permissions/filesystem.h"

#include <algorithm>
#include <system_error>

#include "network/json_io.h"
#include "workspace/environment.h"

namespace ursa {

namespace {

    std::optional<FilesystemRequest::Operation> operation_for(
        std::string_view tool)
    {
        if (tool == "read") {
            return FilesystemRequest::Operation::READ;
        }
        if (tool == "list") {
            return FilesystemRequest::Operation::LIST;
        }
        if (tool == "edit") {
            return FilesystemRequest::Operation::EDIT;
        }
        if (tool == "write") {
            return FilesystemRequest::Operation::WRITE;
        }
        return std::nullopt;
    }

    bool contains(
        const std::filesystem::path& root, const std::filesystem::path& target)
    {
        if (root.empty()) {
            return false;
        }
        const std::filesystem::path relative = target.lexically_relative(root);
        return target == root
            || (!relative.empty() && *relative.begin() != "..");
    }

    bool matches_path(const PermissionStore::Grants& grants,
        PathGrant::Access access, const std::filesystem::path& target)
    {
        return std::any_of(
            grants.begin(), grants.end(), [&](const auto& grant) {
                const auto* path = std::get_if<PathGrant>(&grant);
                if (path == nullptr || path->access != access) {
                    return false;
                }
                return path->target == PathGrant::Target::DIRECTORY
                    ? contains(path->path, target)
                    : path->path == target;
            });
    }

    FilesystemEvaluation reject(std::string reason)
    {
        return { { PermissionDecision::Kind::REJECT, std::move(reason) },
            std::nullopt };
    }

} // namespace

PermissionContext permission_context(const Environment& environment,
    const PermissionStore& permissions, Session::Mode mode)
{
    return { environment.system(), environment.workspace(),
        permissions.snapshot(), mode };
}

FilesystemEvaluation evaluate_filesystem_request(std::string_view tool,
    const std::string& arguments, const PermissionContext& context)
{
    if (!context.system || !context.workspace || !context.grants) {
        return reject("filesystem environment is not ready");
    }
    const auto operation = operation_for(tool);
    if (!operation) {
        return reject("unsupported filesystem tool");
    }
    Json::Value normalized = parse_json(arguments);
    if (!normalized.isObject()) {
        return reject(std::string(tool) + ": arguments must be an object");
    }
    const char* key = *operation == FilesystemRequest::Operation::EDIT
            || *operation == FilesystemRequest::Operation::WRITE
        ? "file_path"
        : "path";
    std::string raw;
    if (normalized[key].isString()) {
        raw = normalized[key].asString();
    } else if (*operation == FilesystemRequest::Operation::LIST
        && normalized[key].isNull()) {
        raw = ".";
    } else {
        return reject(std::string(tool) + ": invalid path argument");
    }
    if (raw.empty() && *operation == FilesystemRequest::Operation::LIST) {
        raw = ".";
    }
    const std::filesystem::path& working_directory
        = context.workspace->working_directory;
    if (raw.empty() || working_directory.empty()) {
        return reject(std::string(tool) + ": invalid path argument");
    }

    std::filesystem::path target = raw;
    if (target.is_relative()) {
        target = working_directory / target;
    }
    std::error_code error;
    target = std::filesystem::weakly_canonical(target, error);
    if (error || !target.is_absolute()) {
        return reject(std::string(tool) + ": cannot normalize target");
    }
    const bool exists = std::filesystem::exists(target, error);
    if (error) {
        return reject(std::string(tool) + ": cannot inspect target");
    }
    if ((*operation == FilesystemRequest::Operation::READ
            || *operation == FilesystemRequest::Operation::EDIT)
        && (!exists || !std::filesystem::is_regular_file(target, error))) {
        return reject(std::string(tool) + ": target is not a file");
    }
    if (*operation == FilesystemRequest::Operation::LIST
        && (!exists || !std::filesystem::is_directory(target, error))) {
        return reject("list: target is not a directory");
    }
    if (*operation == FilesystemRequest::Operation::WRITE) {
        if (exists && !std::filesystem::is_regular_file(target, error)) {
            return reject("write: target is not a file");
        }
        if (!exists
            && ((normalized["overwrite"].isBool()
                    && normalized["overwrite"].asBool())
                || !std::filesystem::is_directory(
                    target.parent_path(), error))) {
            return reject("write: target parent is not a directory");
        }
    }
    if (error) {
        return reject(std::string(tool) + ": cannot inspect target");
    }

    normalized[key] = target.string();
    FilesystemRequest request { *operation, target, std::move(normalized) };
    if (*operation == FilesystemRequest::Operation::LIST) {
        return { { PermissionDecision::Kind::ACCEPT, "" }, request };
    }

    const bool write = *operation == FilesystemRequest::Operation::EDIT
        || *operation == FilesystemRequest::Operation::WRITE;
    const PathGrant::Access access
        = write ? PathGrant::Access::WRITE : PathGrant::Access::READ;
    const bool granted = matches_path(*context.grants, access, target);
    const std::filesystem::path& project_root = context.workspace->project_root
        ? *context.workspace->project_root
        : working_directory;
    const bool trusted                        = contains(project_root, target)
        || contains(context.system->temporary_directory, target);
    if (granted
        || (trusted && (!write || context.mode == Session::Mode::BUILD))) {
        return { { PermissionDecision::Kind::ACCEPT, "" }, request };
    }
    return { { PermissionDecision::Kind::ASK,
                 write ? "write access requires approval"
                       : "read access requires approval" },
        request };
}

std::vector<PermissionGrant> filesystem_session_grants(
    const FilesystemRequest& request)
{
    if (request.operation == FilesystemRequest::Operation::LIST) {
        return { };
    }
    const bool write = request.operation == FilesystemRequest::Operation::EDIT
        || request.operation == FilesystemRequest::Operation::WRITE;
    return { PathGrant {
        write ? PathGrant::Access::WRITE : PathGrant::Access::READ,
        PathGrant::Target::FILE, request.target } };
}

} // namespace ursa
