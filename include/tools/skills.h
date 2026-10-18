#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/imza_signal.h"
#include "common/tool_call.h"
#include "common/types.h"
#include "network/json.h"
#include "tools/tool_args.h"
#include "workspace/attachments.h"
#include "workspace/skills.h"

namespace imza {

struct Config;

// nullopt with `reason` set when the read fails or the size limit is hit.
std::optional<std::string> load_skill_checked(
    const Skill& skill, std::string& reason);
std::optional<std::filesystem::path> canonical_skill_path(const Skill& skill);

// The skill's canonical path iff `request` binds exactly that path.
// Shared by the manual /skill flow and the runner's TOCTOU guard.
std::optional<std::filesystem::path> authorized_skill_path(
    const Skill& skill, const ToolCallRequest& request);
std::optional<Skill> resolve_skill(
    const std::vector<Skill>& catalog, const SkillToolArgs& args);
SkillPolicy skill_policy(const Config& config, const Skill& skill);
std::vector<Skill> mentioned_skills(
    const std::vector<Skill>& catalog, std::string_view text);
std::vector<Skill> allowed_skills(
    const std::vector<Skill>& catalog, const Config& config);

} // namespace imza
