#pragma once

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "common/types.h"
#include "common/ursa_signal.h"
#include "core/git.h"
#include "subsystems/skills.h"

namespace ursa {

class SystemEnvironment {
public:
    explicit SystemEnvironment();
    std::string os_name;
    std::string os_version;
    std::string default_shell;
    std::vector<std::string> package_managers;
    std::string today;
    std::unordered_map<std::string, Skill> global_skills;
    bool has_git { false };
};

class WorkspaceEnvironment {
public:
    explicit WorkspaceEnvironment(const std::filesystem::path& p);

    std::optional<std::filesystem::path> project_root;
    std::optional<InstructionFile> instruction;
    std::unordered_map<std::string, Skill> project_skills;
};

class Environment {
public:
    explicit Environment();
    std::shared_ptr<const SystemEnvironment> system() const { return system_; }
    std::shared_ptr<const WorkspaceEnvironment> workspace() const
    {
        std::shared_lock lock(workspace_mutex_);
        return workspace_;
    }
    std::shared_ptr<const RepositoryState> repository() const
    {
        std::shared_lock lock(workspace_mutex_);
        return repository_;
    }

    // True once the workspace scan has finished, whether or not a project
    // root was found.
    bool ready() const { return ready_.load(); }

    std::optional<std::string> agent_rules_path() const;
    std::vector<Skill> skills() const;

    bool chdir(const std::filesystem::path& dir);
    [[nodiscard]] Signal<>::Subscription subscribe_to_workspace_change(
        Signal<>::Callback callback);
    [[nodiscard]] Signal<>::Subscription subscribe_to_repository_change(
        Signal<>::Callback callback);

private:
    void _publish_workspace(std::shared_ptr<const WorkspaceEnvironment> ws,
        std::uint64_t generation);
    void _publish_repository(std::shared_ptr<const RepositoryState> repository,
        const std::shared_ptr<const WorkspaceEnvironment>& workspace);
    std::shared_ptr<const SystemEnvironment> system_;
    mutable std::shared_mutex workspace_mutex_;
    std::shared_ptr<const WorkspaceEnvironment> workspace_;
    std::shared_ptr<const RepositoryState> repository_;
    Signal<> workspace_changed_;
    Signal<> repository_changed_;
    std::uint64_t workspace_generation_ { 0 };
    std::atomic<bool> ready_ { false };
    std::condition_variable_any workspace_ready_cv_;
    std::jthread worker_;

    std::jthread git_worker_;
};

} // namespace ursa
