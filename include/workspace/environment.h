#pragma once

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/imza_signal.h"
#include "common/types.h"
#include "tools/skills.h"
#include "workspace/git.h"

namespace imza {

struct SystemEnvironment {
    std::string os_name;
    std::string os_version;
    std::string default_shell;
    std::vector<std::string> package_managers;
    std::string today;
    std::unordered_map<std::string, Skill> global_skills;
    std::filesystem::path temporary_directory;
    bool has_git { false };
    std::string clipboard_tool;
};

struct InstructionFile {
    std::string path;
    std::string content;
};

std::optional<InstructionFile> load_agent_file(
    const std::filesystem::path& root);

// Reads one instruction file, relative to `root` unless absolute. Empty
// when the file is missing or unreadable.
std::optional<InstructionFile> load_instruction_file(
    const std::filesystem::path& root, const std::filesystem::path& entry);

struct WorkspaceEnvironment {
    std::filesystem::path working_directory;
    std::optional<std::filesystem::path> project_root;
    std::optional<InstructionFile> instruction;
    // Extra instruction files configured through `config.instructions`,
    // loaded after the agent file and rendered in the same order.
    std::vector<InstructionFile> extra_instructions;
    std::unordered_map<std::string, Skill> project_skills;
};

SystemEnvironment detect_system_environment();
void detect_package_managers(std::vector<std::string>& package_managers);
std::filesystem::path prepare_imza_temporary_directory(
    const std::filesystem::path& base);
// `instructions` are extra files to load alongside the agent file: paths
// are project-root-relative unless absolute, and missing files are
// skipped rather than failing the scan.
WorkspaceEnvironment scan_workspace(const std::filesystem::path& directory,
    const std::vector<std::string>& instructions = { });

class Environment final : public ApplicationComponent {
public:
    enum class ChdirResult { CHANGED, UNCHANGED, FAILED };

    explicit Environment(std::vector<std::string> instructions = { });
    std::shared_ptr<const SystemEnvironment> system() const { return _system; }
    std::shared_ptr<const WorkspaceEnvironment> workspace() const
    {
        std::shared_lock lock(_workspace_mutex);
        return _workspace;
    }
    std::shared_ptr<const RepositoryState> repository() const
    {
        std::shared_lock lock(_workspace_mutex);
        return _repository;
    }

    // True once the workspace scan has finished, whether or not a project
    // root was found.
    bool ready() const { return _ready.load(); }

    std::optional<std::string> agent_rules_path() const;
    std::vector<Skill> skills() const;
    ChdirResult chdir(const std::filesystem::path& dir);
    [[nodiscard]] Signal<>::Subscription subscribe_to_workspace_change(
        Signal<>::Callback callback);
    [[nodiscard]] Signal<>::Subscription subscribe_to_repository_change(
        Signal<>::Callback callback);
    [[nodiscard]] Signal<>::Subscription subscribe_to_update_change(
        Signal<>::Callback callback);

    // Version of an installable newer release, or nullopt.
    std::optional<std::string> update_available() const;

    // Publishes the cached result and refreshes it in the background when
    // `update.json` is older than a day. No-op after the first call.
    void check_for_updates(std::string binary_version);

private:
    void _publish_workspace(std::shared_ptr<const WorkspaceEnvironment> ws,
        std::uint64_t generation);
    const std::vector<std::string> _instructions;
    void _publish_repository(std::shared_ptr<const RepositoryState> repository,
        const std::shared_ptr<const WorkspaceEnvironment>& workspace);
    std::shared_ptr<const SystemEnvironment> _system;
    mutable std::shared_mutex _workspace_mutex;
    std::shared_ptr<const WorkspaceEnvironment> _workspace;
    std::shared_ptr<const RepositoryState> _repository;
    Signal<> _workspace_changed;
    Signal<> _repository_changed;
    Signal<> _update_changed;
    mutable std::mutex _update_mutex;
    std::optional<std::string> _update_version;
    std::atomic<bool> _update_checked { false };
    std::uint64_t _workspace_generation { 0 };
    std::atomic<bool> _ready { false };
    std::condition_variable_any _workspace_ready_cv;
    std::jthread _worker;
    std::jthread _update_worker;

    std::jthread _git_worker;
};

} // namespace imza
