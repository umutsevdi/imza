#pragma once

#include <filesystem>
#include <future>
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

struct Environment {
    std::string os_name;
    std::string os_version;
    std::string default_shell;
    std::vector<std::string> package_managers;
    std::string today;
    std::optional<InstructionFile> instruction;
    std::unordered_map<std::string, std::filesystem::path> project_skills;
    std::unordered_map<std::string, std::filesystem::path> global_skills;
    bool has_git;
    std::optional<std::filesystem::path> project_root;

    bool chdir(const std::filesystem::path& dir);
};

Environment analyze_environment();
std::shared_future<Environment> analyze_environment_async();

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

class EnvironmentV2 {
public:
    explicit EnvironmentV2();
    std::shared_ptr<const SystemEnvironment> system() const { return system_; };
    std::shared_ptr<const WorkspaceEnvironment> workspace() const
    {
        std::shared_lock lock(workspace_mutex_);
        return workspace_;
    };

    bool chdir(const std::filesystem::path& dir);
    void subscribe_to_workspace_change(
        const std::function<void(std::shared_ptr<const WorkspaceEnvironment>)>&
            cb);

private:
    std::shared_ptr<const SystemEnvironment> system_;
    mutable std::shared_mutex workspace_mutex_;
    std::shared_ptr<const WorkspaceEnvironment> workspace_;
    std::vector<
        std::function<void(std::shared_ptr<const WorkspaceEnvironment>)>>
        cbs_;

    std::jthread worker_;
};

// Returns a pointer to environment;
std::shared_ptr<EnvironmentV2> get_environment();

} // namespace ursa
