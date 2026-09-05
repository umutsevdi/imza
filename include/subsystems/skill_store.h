#pragma once

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/types.h"
#include "subsystems/attachments.h"
#include "subsystems/skills.h"

namespace Json {
class Value;
}

namespace ursa {

struct Config;

struct SkillRead {
    enum class Kind { OK, READ_FAILED, TOO_LARGE };
    Kind kind = Kind::OK;
    std::string body;
};

struct PendingSkillTurn {
    std::string text;
    std::vector<FileAttachment> attachments;
    std::vector<Skill> awaiting;
    std::size_t next = 0;
};

SkillRead read_skill(const Skill& skill);
std::vector<std::string> skill_mention_names(std::string_view text);
std::optional<Skill> resolve_skill(
    const std::vector<Skill>& catalog, const Json::Value& args);
SkillPolicy skill_policy(const Config& config, const Skill& skill);
std::vector<Skill> mentioned_skills(
    const std::vector<Skill>& catalog, std::string_view text);
std::vector<Skill> allowed_skills(
    const std::vector<Skill>& catalog, const Config& config);

class SkillStore {
public:
    bool is_loaded(const std::filesystem::path& path) const;
    bool load(const Skill& skill, std::string& error);
    void record_tool_load(const std::filesystem::path& path, std::string body);
    std::string prompt_suffix() const;
    std::pair<SkillCounts, SkillCounts> counts(
        const std::vector<Skill>& catalog) const;
    void clear();
    PendingSkillTurn* pending_turn();
    void set_pending_turn(PendingSkillTurn turn);
    std::optional<PendingSkillTurn> take_pending_turn();

private:
    mutable std::mutex mutex_;
    std::set<std::string> loaded_;
    std::map<std::string, std::string> contents_;
    std::optional<PendingSkillTurn> pending_turn_;
};

} // namespace ursa
