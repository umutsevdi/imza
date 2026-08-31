#include "agent/subsystems/skill_store.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>

#include <json/json.h>

#include "core/config.h"
#include "common/util.h"

namespace ursa {

namespace {

    constexpr std::size_t MAX_SKILL_BYTES = 128 * 1024;

}

SkillRead read_skill(const Skill& skill)
{
    std::ifstream file(skill.path, std::ios::binary);
    if (!file) {
        return { SkillRead::Kind::READ_FAILED, "" };
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    if (content.size() > MAX_SKILL_BYTES) {
        return { SkillRead::Kind::TOO_LARGE, "" };
    }
    return { SkillRead::Kind::OK,
        "<skill name=\"" + skill.name + "\" directory=\""
            + skill.path.parent_path().string() + "\">\n" + content
            + "\n</skill>" };
}

std::vector<std::string> skill_mention_names(std::string_view text)
{
    std::vector<std::string> names;
    for (std::size_t pos = 0; pos < text.size();) {
        pos = text.find('$', pos);
        if (pos == std::string_view::npos) {
            break;
        }
        if (pos > 0
            && !std::isspace(static_cast<unsigned char>(text[pos - 1]))) {
            ++pos;
            continue;
        }
        const std::size_t end = mention_end(text, pos);
        if (end > pos + 1) {
            names.emplace_back(text.substr(pos + 1, end - pos - 1));
        }
        pos = end;
    }
    return names;
}

std::optional<Skill> resolve_skill(
    const std::vector<Skill>& catalog, const Json::Value& args)
{
    if (!args.isObject() || !args["name"].isString()) {
        return std::nullopt;
    }
    const std::string name = args["name"].asString();
    const std::string scope
        = args["scope"].isString() ? args["scope"].asString() : "";
    std::optional<Skill> global;
    for (const Skill& skill : catalog) {
        if (skill.name != name) {
            continue;
        }
        if (scope == "project" && skill.scope != Skill::Scope::PROJECT) {
            continue;
        }
        if (scope == "global" && skill.scope != Skill::Scope::GLOBAL) {
            continue;
        }
        if (skill.scope == Skill::Scope::PROJECT) {
            return skill;
        }
        global = skill;
    }
    return global;
}

SkillPolicy skill_policy(const Config& config, const Skill& skill)
{
    if (skill.scope == Skill::Scope::GLOBAL) {
        if (auto it = config.global_skills.find(skill.name);
            it != config.global_skills.end()) {
            return it->second;
        }
    } else if (skill.project_root) {
        if (auto project
            = config.project_skills.find(skill.project_root->string());
            project != config.project_skills.end()) {
            if (auto it = project->second.find(skill.name);
                it != project->second.end()) {
                return it->second;
            }
        }
    }
    return SkillPolicy::ASK;
}

std::vector<Skill> mentioned_skills(
    const std::vector<Skill>& catalog, std::string_view text)
{
    std::vector<Skill> out;
    std::set<std::string> paths;
    for (const std::string& name : skill_mention_names(text)) {
        Json::Value args(Json::objectValue);
        args["name"] = name;
        if (const auto skill = resolve_skill(catalog, args);
            skill && paths.insert(skill->path.string()).second) {
            out.push_back(*skill);
        }
    }
    return out;
}

std::vector<Skill> allowed_skills(
    const std::vector<Skill>& catalog, const Config& config)
{
    std::vector<Skill> out;
    std::copy_if(catalog.begin(), catalog.end(), std::back_inserter(out),
        [&](const Skill& skill) {
            return skill_policy(config, skill) != SkillPolicy::DENY;
        });
    return out;
}

bool SkillStore::is_loaded(const std::filesystem::path& path) const
{
    std::lock_guard lock(mutex_);
    return loaded_.contains(path.string());
}

bool SkillStore::load(const Skill& skill, std::string& error)
{
    if (is_loaded(skill.path)) {
        return true;
    }
    const SkillRead read = read_skill(skill);
    if (read.kind == SkillRead::Kind::READ_FAILED) {
        error = "Failed to read skill: " + skill.name + ".";
        return false;
    }
    if (read.kind == SkillRead::Kind::TOO_LARGE) {
        error = "Skill instructions exceed 128 KiB: " + skill.name + ".";
        return false;
    }
    std::lock_guard lock(mutex_);
    loaded_.insert(skill.path.string());
    contents_[skill.path.string()] = read.body;
    return true;
}

void SkillStore::record_tool_load(
    const std::filesystem::path& path, std::string body)
{
    std::lock_guard lock(mutex_);
    loaded_.insert(path.string());
    contents_[path.string()] = std::move(body);
}

std::string SkillStore::prompt_suffix() const
{
    std::lock_guard lock(mutex_);
    std::string out;
    for (const auto& [path, content] : contents_) {
        out += "\n\n" + content;
    }
    return out;
}

std::pair<SkillCounts, SkillCounts> SkillStore::counts(
    const std::vector<Skill>& catalog) const
{
    SkillCounts project;
    SkillCounts global;
    std::lock_guard lock(mutex_);
    for (const Skill& skill : catalog) {
        SkillCounts& scope
            = skill.scope == Skill::Scope::PROJECT ? project : global;
        ++scope.total;
        if (loaded_.contains(skill.path.string())) {
            ++scope.active;
        }
    }
    return { project, global };
}

void SkillStore::clear()
{
    std::lock_guard lock(mutex_);
    loaded_.clear();
    contents_.clear();
    pending_turn_.reset();
}

PendingSkillTurn* SkillStore::pending_turn()
{
    return pending_turn_ ? &*pending_turn_ : nullptr;
}

void SkillStore::set_pending_turn(PendingSkillTurn turn)
{
    pending_turn_ = std::move(turn);
}

std::optional<PendingSkillTurn> SkillStore::take_pending_turn()
{
    std::optional<PendingSkillTurn> turn = std::move(pending_turn_);
    pending_turn_.reset();
    return turn;
}

} // namespace ursa
