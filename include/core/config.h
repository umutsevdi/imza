#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "common/modal.h"
#include "common/types.h"

namespace ursa {

struct Connection {
    std::string id;
    std::string provider_id;
    std::string endpoint;
    std::string api_key;
    std::string label;
    std::map<std::string, ApiStandard> dialects;
};

struct LastUsed {
    std::string provider;
    std::string model;
};

enum class SubagentRole { BUILDER, RESEARCH, BASIC };

struct SubagentModelConfig {
    std::string provider;
    std::string model;
    std::string variant;
};

struct Config {
    std::vector<Connection> providers;
    std::optional<LastUsed> last_used;
    std::optional<std::string> reasoning_effort;
    std::map<SubagentRole, SubagentModelConfig> subagents;
    std::map<std::string, SkillPolicy> global_skills;
    std::map<std::string, std::map<std::string, SkillPolicy>> project_skills;
};

Status load_config(const std::filesystem::path& path, Config& out,
    std::string* error = nullptr);
Status save_config(const std::filesystem::path& path, const Config& cfg);
void apply_skill_policies(Config& config, const SkillPolicyChanges& changes);
std::filesystem::path base_config_dir(void);
std::filesystem::path config_path(void);
std::filesystem::path presets_path(void);
std::filesystem::path data_dir(void);
std::filesystem::path sessions_dir(void);

} // namespace ursa
