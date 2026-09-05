#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

namespace ursa {

struct InstructionFile {
    std::string path;
    std::string content;
};

struct Skill {
    enum class Scope { GLOBAL, PROJECT };
    std::string name;
    std::string description;
    std::filesystem::path path;
    Scope scope = Scope::GLOBAL;
    std::optional<std::filesystem::path> project_root;
};

struct SkillCounts {
    std::size_t active = 0;
    std::size_t total  = 0;
};

std::optional<InstructionFile> load_agent_file(
    const std::filesystem::path& root);

} // namespace ursa
