#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
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
    std::optional<InstructionFile> instruction;
    std::unordered_map<std::string, std::filesystem::path> project_skills;
};

struct ChangedFile {
    enum class Kind {
        MODIFIED,
        ADDED,
        UNTRACKED,
        DELETED,
        RENAMED,
        COPIED,
        CONFLICTED,
        UNKNOWN,
    };

    std::string path;
    Kind kind = Kind::UNKNOWN;

    bool operator==(const ChangedFile&) const = default;
};

std::vector<ChangedFile> parse_git_status(std::string_view status);
std::string normalize_git_branch(std::string_view branch);

struct RepositoryState {
    std::string branch;
    std::vector<ChangedFile> changed_files;
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
    std::size_t project_skills() const;
    std::size_t global_skills() const;

    bool chdir(const std::filesystem::path& dir);
    std::function<void()> subscribe_to_workspace_change(
        const std::function<void()>& cb);
    std::function<void()> subscribe_to_repository_change(
        const std::function<void()>& cb);

private:
    struct Subscriber {
        std::uint64_t id;
        std::function<void()> cb;
    };

    void _publish_workspace(std::shared_ptr<const WorkspaceEnvironment> ws,
        std::uint64_t generation);
    void _publish_repository(std::shared_ptr<const RepositoryState> repository,
        const std::shared_ptr<const WorkspaceEnvironment>& workspace);
    std::function<void()> _subscribe(
        std::vector<Subscriber>& subscribers, const std::function<void()>& cb,
        bool workspace_events);
    void _notify(const std::vector<Subscriber>& subscribers);

    std::shared_ptr<const SystemEnvironment> system_;
    mutable std::shared_mutex workspace_mutex_;
    std::shared_ptr<const WorkspaceEnvironment> workspace_;
    std::shared_ptr<const RepositoryState> repository_;
    std::vector<Subscriber> workspace_cbs_;
    std::vector<Subscriber> repository_cbs_;
    std::uint64_t next_id_ { 1 };
    std::uint64_t workspace_generation_ { 0 };
    std::atomic<bool> ready_ { false };
    std::condition_variable_any workspace_ready_cv_;
    std::jthread worker_;

    std::jthread git_worker_;
};

std::string shell_name(const SystemEnvironment& sys);

// Returns the process-wide environment singleton.
std::shared_ptr<Environment> get_environment();

} // namespace ursa
