#pragma once

#include <chrono>
#include <filesystem>
#include <future>
#include <optional>
#include <string>
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
    std::string distro;
    std::string default_shell;
    std::vector<std::string> package_managers;
    std::string today;
    std::optional<InstructionFile> instruction;
    std::unordered_map<std::string, std::filesystem::path> project_skills;
    std::unordered_map<std::string, std::filesystem::path> global_skills;
    bool has_git;
};

Environment analyze_environment();
std::shared_future<Environment> analyze_environment_async();
std::optional<InstructionFile> load_agent_file(
    const std::filesystem::path& root);

} // namespace ursa
