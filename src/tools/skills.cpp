#include "tools/skills.h"

#include <algorithm>
#include <cctype>

#include "common/util.h"
#include "network/json.h"
#include "platform/config.h"
#include "platform/json_file.h"

namespace imza {

namespace {

    constexpr std::size_t MAX_SKILL_BYTES = 128 * 1024;

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

} // namespace

std::optional<std::string> load_skill_checked(
    const Skill& skill, std::string& reason)
{
    const std::optional<std::string> content = read_text_file(skill.path);
    if (!content) {
        reason = "cannot read instructions";
        return std::nullopt;
    }
    if (content->size() > MAX_SKILL_BYTES) {
        reason = "instructions exceed " + std::to_string(MAX_SKILL_BYTES / 1024)
            + " KiB";
        return std::nullopt;
    }
    return "<skill name=\"" + skill.name + "\" directory=\""
        + skill.path.parent_path().string() + "\">\n" + *content + "\n</skill>";
}

std::optional<std::filesystem::path> canonical_skill_path(const Skill& skill)
{
    std::error_code error;
    std::filesystem::path path
        = std::filesystem::weakly_canonical(skill.path, error);
    if (error || !path.is_absolute()) {
        return std::nullopt;
    }
    return path;
}
std::optional<std::filesystem::path> authorized_skill_path(
    const Skill& skill, const ToolCallRequest& request)
{
    const std::optional<std::filesystem::path> path
        = canonical_skill_path(skill);
    SkillToolArgs arguments;
    if (json_parse_checked(request.args, arguments)) {
        return std::nullopt;
    }
    if (!path || !arguments.path || *arguments.path != path->string()) {
        return std::nullopt;
    }
    return path;
}

std::optional<Skill> resolve_skill(
    const std::vector<Skill>& catalog, const SkillToolArgs& args)
{
    const std::string name  = args.name;
    const std::string scope = args.scope.value_or("");
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
        SkillToolArgs args;
        args.name = name;
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
    std::lock_guard lock(_mutex);
    return _loaded.contains(path.string());
}

bool SkillStore::load(const Skill& skill, std::string& error)
{
    if (is_loaded(skill.path)) {
        return true;
    }
    std::string reason;
    const std::optional<std::string> body = load_skill_checked(skill, reason);
    if (!body) {
        error = "Cannot load skill '" + skill.name + "': " + reason + ".";
        return false;
    }
    std::lock_guard lock(_mutex);
    _loaded.insert(skill.path.string());
    _contents[skill.path.string()] = std::move(*body);
    return true;
}

void SkillStore::record_tool_load(
    const std::filesystem::path& path, std::string body)
{
    std::lock_guard lock(_mutex);
    _loaded.insert(path.string());
    _contents[path.string()] = std::move(body);
}

std::string SkillStore::prompt_suffix() const
{
    std::lock_guard lock(_mutex);
    std::string out;
    for (const auto& [path, content] : _contents) {
        out += "\n\n" + content;
    }
    return out;
}

std::pair<SkillCounts, SkillCounts> SkillStore::counts(
    const std::vector<Skill>& catalog) const
{
    SkillCounts project;
    SkillCounts global;
    std::lock_guard lock(_mutex);
    for (const Skill& skill : catalog) {
        SkillCounts& scope
            = skill.scope == Skill::Scope::PROJECT ? project : global;
        ++scope.total;
        if (_loaded.contains(skill.path.string())) {
            ++scope.active;
        }
    }
    return { project, global };
}

void SkillStore::clear()
{
    std::lock_guard lock(_mutex);
    _loaded.clear();
    _contents.clear();
    _pending_turn.reset();
}

std::optional<PendingSkillTurn> SkillStore::pending_turn() const
{
    std::lock_guard lock(_mutex);
    return _pending_turn;
}

void SkillStore::set_pending_turn(PendingSkillTurn turn)
{
    std::lock_guard lock(_mutex);
    _pending_turn = std::move(turn);
}

std::optional<PendingSkillTurn> SkillStore::advance_pending_turn()
{
    std::lock_guard lock(_mutex);
    if (!_pending_turn) {
        return std::nullopt;
    }
    ++_pending_turn->next;
    return _pending_turn;
}

std::optional<PendingSkillTurn> SkillStore::take_pending_turn()
{
    std::lock_guard lock(_mutex);
    std::optional<PendingSkillTurn> turn = std::move(_pending_turn);
    _pending_turn.reset();
    return turn;
}

} // namespace imza
