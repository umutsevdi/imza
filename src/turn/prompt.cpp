#include "turn/prompt.h"
#include "app/application_state.h"
#include "common/util.h"
#include "conversation/session.h"
#include "platform/json_file.h"
#include "tools/mcp_manager.h"
#include "tools/skills.h"

#include <algorithm>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "prompt_defaults.inc"

namespace imza {

namespace {

    bool has_content(std::string_view text) { return !trim(text).empty(); }

    std::string strip_trailing_space(std::string text)
    {
        while (!text.empty()
            && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '
                || text.back() == '\t')) {
            text.pop_back();
        }
        return text;
    }

    std::string load_prompt(const std::filesystem::path& overrides,
        std::string_view file_name, std::string_view fallback)
    {
        if (!overrides.empty()) {
            std::optional<std::string> text
                = read_text_file(overrides / file_name);
            if (text && has_content(*text)) {
                return strip_trailing_space(std::move(*text));
            }
        }
        return std::string(fallback);
    }

    std::string environment_block(
        const SystemEnvironment& sys, const WorkspaceEnvironment* ws)
    {
        std::string out = "<env>";
        out += "\n  Current Directory: ";
        out += ws == nullptr ? "unknown" : ws->working_directory.string();
        if (ws != nullptr && ws->project_root.has_value()) {
            out += "\n  Project Root: " + ws->project_root.value().string();
        }
        out += "\n  Operating System: ";
        out += sys.os_name;
        if (!sys.os_version.empty()) {
            out += " ";
            out += sys.os_version;
        }
        out += "\n  Shell: ";
        out += sys.default_shell;
        out += "\n  Package managers: ";
        out += sys.package_managers.empty() ? std::string("none")
                                            : join(sys.package_managers, ", ");
        out += "\n  Today's date: ";
        out += sys.today;
        out += "\n</env>";
        return out;
    }

    std::string instructions_block(const InstructionFile& file)
    {
        std::string out = "<instructions source=\"";
        out += file.path;
        out += "\">\n";
        out += file.content;
        if (out.back() != '\n') {
            out += '\n';
        }
        out += "</instructions>";
        return out;
    }

    void append_context(std::string& out, const SystemEnvironment* sys,
        const WorkspaceEnvironment* ws, const Config* config)
    {
        if (sys == nullptr) {
            return;
        }
        out += "\n\n";
        out += environment_block(*sys, ws);
        if (ws != nullptr && ws->instruction) {
            out += "\n\n";
            out += instructions_block(*ws->instruction);
        }
        if (ws != nullptr) {
            for (const InstructionFile& file : ws->extra_instructions) {
                out += "\n\n";
                out += instructions_block(file);
            }
        }
        std::vector<Skill> skills;
        for (const auto& [name, skill] : sys->global_skills) {
            skills.push_back(skill);
        }
        if (ws != nullptr) {
            for (const auto& [name, skill] : ws->project_skills) {
                skills.push_back(skill);
            }
        }
        std::sort(
            skills.begin(), skills.end(), [](const Skill& a, const Skill& b) {
                if (a.scope != b.scope) {
                    return a.scope == Skill::Scope::PROJECT;
                }
                return a.name < b.name;
            });
        std::string catalog;
        for (const Skill& skill : skills) {
            const SkillPolicy policy = config == nullptr
                ? SkillPolicy::ASK
                : skill_policy(*config, skill);
            if (policy == SkillPolicy::DENY) {
                continue;
            }
            catalog += "- ";
            catalog += skill.name + " ["
                + (skill.scope == Skill::Scope::PROJECT ? "project" : "global")
                + "]";
            if (!skill.description.empty()) {
                catalog += ": " + skill.description + "\n";
            }
        }
        if (!catalog.empty()) {
            out += "\n\n<skills>\n";
            out += catalog;
            out += "\n</skills>";
        }
    }

    // One line per configured server; tool descriptions stay behind
    // load_mcp. Rendered only when servers are configured.
    std::string mcp_block(const McpManager* mcp)
    {
        if (mcp == nullptr) {
            return "";
        }
        std::string catalog;
        for (const McpServerSnapshot& server : mcp->snapshot()) {
            catalog += "- " + server.id;
            if (!server.label.empty() && server.label != server.id) {
                catalog += " (" + server.label + ")";
            }
            switch (server.state) {
            case McpServerState::CONNECTED:
                catalog += " [connected, " + std::to_string(server.tool_count)
                    + " tools]";
                break;
            case McpServerState::CONNECTING: catalog += " [connecting]"; break;
            case McpServerState::FAILED: catalog += " [failed]"; break;
            case McpServerState::DISABLED: catalog += " [disabled]"; break;
            case McpServerState::OFFLINE: catalog += " [offline]"; break;
            }
            if (!server.description.empty()) {
                catalog += ": " + server.description;
            }
            catalog += "\n";
        }
        if (catalog.empty()) {
            return "";
        }
        std::string out = "<mcps>\n";
        out += catalog;
        out += "load_mcp(\"<id>\") returns a server's tool reference; ";
        out += "call tools with imza.mcp.call(\"<id>\", \"<tool>\", {args}).\n";
        out += "</mcps>";
        return out;
    }

} // namespace

PromptStore::PromptStore(const std::filesystem::path& overrides)
    : _system(load_prompt(overrides, "system.md", prompts_detail::SYSTEM))
    , _subagent(load_prompt(overrides, "subagent.md", prompts_detail::SUBAGENT))
    , _subagent_research(load_prompt(
          overrides, "subagent_research.md", prompts_detail::SUBAGENT_RESEARCH))
    , _subagent_build(load_prompt(
          overrides, "subagent_build.md", prompts_detail::SUBAGENT_BUILD))
    , _title(load_prompt(overrides, "title.md", prompts_detail::TITLE))
    , _compaction(
          load_prompt(overrides, "compaction.md", prompts_detail::COMPACTION))
    , _make_skill(
          load_prompt(overrides, "make_skill.md", prompts_detail::MAKE_SKILL))
    , _review(load_prompt(overrides, "review.md", prompts_detail::REVIEW))
    , _review_plan(
          load_prompt(overrides, "review_plan.md", prompts_detail::REVIEW_PLAN))
    , _plan_annotations(load_prompt(
          overrides, "plan_annotations.md", prompts_detail::PLAN_ANNOTATIONS))
{
}

std::string build_system_prompt(const PromptStore& prompts,
    const SystemEnvironment* sys, const WorkspaceEnvironment* ws,
    const Config* config)
{
    std::string out = prompts.system();
    append_context(out, sys, ws, config);
    return out;
}

std::string build_subagent_system_prompt(const PromptStore& prompts,
    const SystemEnvironment* sys, const WorkspaceEnvironment* ws,
    SubagentRole role, const Config* config, const McpManager* mcp)
{
    if (role == SubagentRole::BASIC) {
        return { };
    }
    std::string out = prompts.subagent();
    out += "\n\n";
    out += role == SubagentRole::RESEARCH ? prompts.subagent_research()
                                          : prompts.subagent_build();
    append_context(out, sys, ws, config);
    const std::string mcps = mcp_block(mcp);
    if (!mcps.empty()) {
        out += "\n\n" + mcps;
    }
    return out;
}

std::string title_prompt(const PromptStore& prompts, std::string_view request)
{
    std::string out = prompts.title();
    out += "\n\nUser request:\n";
    out += request;
    return out;
}

std::string current_mode_prompt(Session::Mode mode)
{
    return mode == Session::Mode::PLAN ? "<runtime-mode name=\"plan\"/>"
                                       : "<runtime-mode name=\"build\"/>";
}

std::string full_system_prompt(
    const ApplicationState& state, std::optional<Session::Mode> mode)
{
    const std::shared_ptr<Environment> env = state.environment;
    const Config config                    = state.providers->config();
    std::string prompt                     = build_system_prompt(
        *state.prompts, env->system().get(), env->workspace().get(), &config);
    const std::string mcps = mcp_block(state.mcp.get());
    if (!mcps.empty()) {
        prompt += "\n\n" + mcps;
    }
    prompt += state.skills->prompt_suffix();
    if (mode) {
        prompt += "\n\n";
        prompt += current_mode_prompt(*mode);
    }
    return prompt;
}

std::string format_plan_annotations_prompt(std::string_view instructions,
    const std::vector<PlanNote>& notes, std::string_view document)
{
    if (notes.empty()) {
        return { };
    }
    std::vector<std::string> lines = split_lines(document);
    // A document ending in '\n' pins one final empty line; keep it so
    // notes on that line resolve instead of turning stale.
    if (document.empty() || document.back() == '\n') {
        lines.emplace_back();
    }
    // Nearest heading above each pinned line: a plain line scan, mirroring
    // the cmark-derived sections without a ui dependency.
    const auto section_of = [&lines](std::size_t line) {
        for (std::size_t l = std::min(line, lines.size()); l >= 1; --l) {
            std::string_view text = lines[l - 1];
            if (text.rfind("#", 0) == 0) {
                while (!text.empty()
                    && (text.back() == '\r' || text.back() == ' ')) {
                    text.remove_suffix(1);
                }
                std::size_t hashes = 0;
                while (hashes < text.size() && text[hashes] == '#') {
                    ++hashes;
                }
                if (hashes < text.size() && text[hashes] == ' ') {
                    return text.substr(hashes + 1);
                }
            }
            if (l == 1) {
                break;
            }
        }
        return std::string_view("Document");
    };
    std::string prompt(instructions);
    for (const PlanNote& note : notes) {
        prompt += "\n\n- `";
        if (note.line == 0 || note.line > lines.size()) {
            prompt += "(stale)";
        } else {
            prompt += section_of(note.line);
            prompt += " > ";
            std::string_view anchor = lines[note.line - 1];
            while (!anchor.empty()
                && (anchor.back() == '\r' || anchor.back() == ' ')) {
                anchor.remove_suffix(1);
            }
            prompt += anchor;
        }
        prompt += "`\n  ";
        for (const char c : note.body) {
            prompt += c;
            if (c == '\n') {
                prompt += "  ";
            }
        }
    }
    return prompt;
}

} // namespace imza
