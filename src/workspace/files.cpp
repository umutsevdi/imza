#include "workspace/files.h"

#include "platform/command_runner.h"

#include <algorithm>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace imza {

namespace {

    bool enumerate_with_git(const FileWalkOptions& options,
        const std::function<bool(const std::filesystem::path&)>& visit,
        bool& truncated)
    {
        const std::string prefix = "git -C " + shell_quote(options.root)
            + " ls-files -co --exclude-standard";
        const CommandResult result
            = run_command(prefix, std::chrono::seconds { 10 });
        if (!command_ok(result)) {
            return false;
        }
        std::istringstream stream(result.output);
        std::string line;
        std::size_t seen = 0;
        while (std::getline(stream, line)) {
            if (line.empty()) {
                continue;
            }
            if (options.limit != 0 && seen >= options.limit) {
                truncated = true;
                return true;
            }
            if (options.deadline != std::chrono::steady_clock::time_point { }
                && std::chrono::steady_clock::now() > options.deadline) {
                truncated = true;
                return true;
            }
            ++seen;
            if (!visit(std::filesystem::path(line))) {
                return true;
            }
        }
        return true;
    }

    bool is_ignored_relative(const std::filesystem::path& relative)
    {
        for (const std::filesystem::path& part : relative) {
            if (ignored_directory(part.string())) {
                return true;
            }
        }
        return false;
    }

    void enumerate_by_walking(const FileWalkOptions& options,
        const std::function<bool(const std::filesystem::path&)>& visit,
        bool& truncated)
    {
        std::error_code ec;
        std::filesystem::recursive_directory_iterator iterator(options.root,
            std::filesystem::directory_options::skip_permission_denied, ec);
        if (ec) {
            return;
        }
        const std::filesystem::recursive_directory_iterator end;
        std::size_t seen = 0;
        while (iterator != end) {
            if (options.limit != 0 && seen >= options.limit) {
                truncated = true;
                return;
            }
            if (options.deadline != std::chrono::steady_clock::time_point { }
                && std::chrono::steady_clock::now() > options.deadline) {
                truncated = true;
                return;
            }
            std::error_code entry_ec;
            const std::filesystem::directory_entry entry = *iterator;
            const std::filesystem::path relative = std::filesystem::relative(
                entry.path(), options.root, entry_ec);
            if (!entry_ec && options.apply_ignores
                && is_ignored_relative(relative)) {
                if (entry.is_directory(entry_ec)) {
                    iterator.disable_recursion_pending();
                }
                iterator.increment(ec);
                if (ec) {
                    ec.clear();
                }
                continue;
            }
            if (entry.is_regular_file(entry_ec) && !entry_ec) {
                ++seen;
                if (!visit(relative)) {
                    return;
                }
            }
            iterator.increment(ec);
            if (ec) {
                ec.clear();
            }
        }
    }

} // namespace

bool ignored_directory(std::string_view name)
{
    static const std::unordered_set<std::string> ignored { ".git", ".hg",
        ".svn", ".cache", ".next", ".nuxt", ".idea", ".vscode", ".venv", "venv",
        "node_modules", "build", "dist", "out", "target", "vendor", "coverage",
        "__pycache__" };
    return ignored.contains(std::string(name));
}

void for_each_file(const FileWalkOptions& options,
    const std::function<bool(const std::filesystem::path&)>& visit,
    bool& truncated)
{
    truncated = false;
    std::error_code ec;
    if (!std::filesystem::is_directory(options.root, ec) || ec) {
        return;
    }
    if (!options.apply_ignores) {
        enumerate_by_walking(options, visit, truncated);
        return;
    }
    if (enumerate_with_git(options, visit, truncated)) {
        return;
    }
    enumerate_by_walking(options, visit, truncated);
}

} // namespace imza
