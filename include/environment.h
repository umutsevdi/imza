#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ursa {

struct InstructionFile {
    std::string path;
    std::string content;
};

std::optional<InstructionFile> load_agent_file(
    const std::filesystem::path& root);

class SystemEnvironment {
public:
    explicit SystemEnvironment();
    std::string os_name;
    std::string os_version;
    std::string default_shell;
    std::vector<std::string> package_managers;
    std::string today;
    std::unordered_map<std::string, std::filesystem::path> global_skills;
    bool has_git { false };
};

class WorkspaceEnvironment {
public:
    explicit WorkspaceEnvironment(const std::filesystem::path& p);

    std::optional<std::filesystem::path> project_root;
    std::string branch;
    std::optional<InstructionFile> instruction;
    std::unordered_map<std::string, std::filesystem::path> project_skills;
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

    // True once the workspace scan has finished, whether or not a project
    // root was found (a non-git folder yields a null workspace but is ready).
    bool ready() const { return ready_.load(); }

    std::optional<std::string> agent_rules_path() const;
    std::size_t project_skills() const;
    std::size_t global_skills() const;

    bool chdir(const std::filesystem::path& dir);
    // Subscribes to workspace changes. Returns a handle that, when invoked,
    // removes the subscription.
    std::function<void()> subscribe_to_workspace_change(
        const std::function<void(std::shared_ptr<const WorkspaceEnvironment>)>&
            cb);

private:
    struct Subscriber {
        std::uint64_t id;
        std::function<void(std::shared_ptr<const WorkspaceEnvironment>)> cb;
    };

    void publish(std::shared_ptr<const WorkspaceEnvironment> ws);

    std::shared_ptr<const SystemEnvironment> system_;
    mutable std::shared_mutex workspace_mutex_;
    std::shared_ptr<const WorkspaceEnvironment> workspace_;
    std::vector<Subscriber> cbs_;
    std::uint64_t next_id_ { 1 };
    std::atomic<bool> ready_ { false };
    std::jthread worker_;
};

std::string shell_name(const SystemEnvironment& sys);

// Returns the process-wide environment singleton.
std::shared_ptr<Environment> get_environment();

} // namespace ursa
