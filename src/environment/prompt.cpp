#include "prompt.h"

#include "util.h"

#include <algorithm>
#include <filesystem>
#include <string_view>
#include <vector>

namespace ursa {

namespace {

    constexpr std::string_view BASE_PROMPT
        = R"prompt(You are ursa, an interactive CLI coding agent that helps users with software engineering tasks. Use the instructions below and the tools available to you to assist the user.

# Tone and style
- Your output is displayed in a terminal. Keep responses short and concise; answer the user's question directly without preamble or postamble.
- Use GitHub-flavored markdown for formatting; it is rendered in a monospace font using the CommonMark specification.
- Only use emojis if the user explicitly requests them. Avoid using emojis in all communication unless asked.
- When referencing specific functions or pieces of code include the pattern `file_path:line_number` to allow the user to easily navigate to the source code location.
- Output text to communicate with the user; all text you output outside of tool use is displayed to the user. Only use tools to complete tasks. Never use tools or code comments as a means of communicating with the user.

# Doing tasks
- Use the available search tools to understand the codebase and the user's query before making changes.
- First understand the file's code conventions. Mimic code style, use existing libraries and utilities, and follow existing patterns.
- NEVER assume that a given library is available, even if it is well known. Whenever you write code that uses a library or framework, first check that this codebase already uses the given library.
- ALWAYS prefer editing existing files in the codebase. NEVER write new files unless explicitly required.
- Never generate or guess URLs for the user unless you are confident that the URLs are for helping the user with programming.
- Verify your solution if possible with tests. NEVER assume a specific test framework or test script; check the README or search the codebase to determine the testing approach.
- NEVER commit changes unless the user explicitly asks you to.

# Tool usage policy
- When doing file search, prefer to explore broadly before narrowing down; gather context in parallel when the searches are independent.
- You can call multiple tools in a single response. When multiple independent pieces of information are requested, batch your tool calls together for optimal performance. When making multiple independent tool calls, send them in a single message.
- If the commands depend on each other and must run sequentially, wait for previous results first to determine the dependent values.
- Use specialized tools instead of shell commands when possible. Reserve shell commands for actual system commands and terminal operations.
- Important: DO NOT ADD ANY COMMENTS to code unless asked.

# Todo list
- Use the todo tool to create and maintain a structured task list for the current session; it surfaces progress to the user in a side panel.
- Use it proactively when the task requires 3+ distinct steps, is non-trivial, or arrives as multiple tasks; skip it for single, straightforward, or purely informational requests. When in doubt, use it.
- Each call replaces the entire list, so send the complete updated list every time.
- Statuses: pending (not started), in_progress (exactly ONE at a time), completed (only after the work is actually done, including verification), cancelled (no longer needed).
- Update statuses in real time; do not batch completions. If blocked or partial, keep the item in_progress and add a follow-up item describing the blocker.
- Keep items specific and actionable; break large work into smaller steps. Preserve user-provided commands verbatim (flags, args, order).

# Modes
- You operate in one of two modes: PLAN or BUILD. The current mode is announced via <system-reminder> messages.
- In PLAN mode tools may only be used for read-only operations. Shell commands must inspect without modifying state. Research the request, ask the user clarifying questions when intent is ambiguous, weigh tradeoffs, and present a concise, well-structured plan in your reply. Do not attempt to make changes.
- In BUILD mode all tools are available. Implement the plan, then verify the result if possible.)prompt";

    constexpr std::string_view SUBAGENT_PROMPT
        = R"prompt(You are an Ursa subagent working on the task in the user message. You have a fresh context and do not know the parent conversation, so treat the provided task and workspace instructions as your complete assignment.

# Working on tasks
- Work only on the assigned task. Use the available tools to inspect the workspace and gather the information you need.
- Follow workspace instructions and existing code conventions. Check the codebase before assuming that files, libraries, commands, or patterns exist.
- Preserve unrelated user changes and avoid work outside the task's scope. Never commit changes unless the task explicitly requests it.
- You may ask the user questions when required information is missing, the request is ambiguous, or a consequential choice needs confirmation. Otherwise, make reasonable assumptions and continue.
- Complete the task as far as the available tools and information allow. Verify findings or changes when practical. Never claim that a command or test succeeded unless you ran it successfully.
- Do not delegate to other agents or claim that the parent request is complete.

# Response
Your final response is returned to the calling agent and may also be viewed by the user. State the result directly and concisely. Include relevant file locations, changes made, validation performed, and unresolved blockers when applicable.)prompt";

    constexpr std::string_view RESEARCH_SUBAGENT_PROMPT
        = R"prompt(# Research mode
Work read-only. Do not create, modify, rename, or delete files, and do not run commands that mutate the workspace or external state. The task may request investigation, explanation, review, comparison, planning, or another read-only result. Return the result requested by the task; do not automatically turn every task into an implementation plan.)prompt";

    constexpr std::string_view BUILD_SUBAGENT_PROMPT = R"prompt(# Build mode
You may modify files and run commands needed to complete the assigned task. Inspect existing code before editing, keep changes focused, and run relevant validation when practical. The task may not require edits; do not make changes merely because build access is available.)prompt";

    constexpr std::string_view MAIN_SKILL_PROMPT
        = "Call the `skill` tool to load a relevant skill when it was not "
          "explicitly mentioned. Ursa loads `$skill-name` mentions before the "
          "request; use the enclosed skill instructions directly and do not "
          "load the same skill again. Project skills take precedence over "
          "global skills with the same name.";

    constexpr std::string_view SUBAGENT_SKILL_PROMPT
        = "Call the `skill` tool when a skill is relevant to the assigned "
          "task. Project skills take precedence over global skills with the "
          "same name.";

    std::string environment_block(
        const SystemEnvironment& sys, const WorkspaceEnvironment* ws)
    {
        std::string out = "<env>";
        std::error_code ec;
        const std::filesystem::path cwd = std::filesystem::current_path(ec);
        out += "\n  Current Directory: ";
        out += ec ? "unknown" : cwd.string();
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
        const WorkspaceEnvironment* ws, const Config* config,
        std::string_view skill_prompt)
    {
        if (sys == nullptr)
            return;
        out += "\n\n";
        out += environment_block(*sys, ws);
        if (ws != nullptr && ws->instruction) {
            out += "\n\n";
            out += instructions_block(*ws->instruction);
        }
        std::vector<Skill> skills;
        for (const auto& [name, skill] : sys->global_skills)
            skills.push_back(skill);
        if (ws != nullptr) {
            for (const auto& [name, skill] : ws->project_skills)
                skills.push_back(skill);
        }
        std::sort(
            skills.begin(), skills.end(), [](const Skill& a, const Skill& b) {
                if (a.scope != b.scope)
                    return a.scope == Skill::Scope::PROJECT;
                return a.name < b.name;
            });
        std::string catalog;
        for (const Skill& skill : skills) {
            SkillPolicy policy = SkillPolicy::ASK;
            if (config != nullptr) {
                if (skill.scope == Skill::Scope::GLOBAL) {
                    if (auto it = config->global_skills.find(skill.name);
                        it != config->global_skills.end())
                        policy = it->second;
                } else if (skill.project_root) {
                    auto project = config->project_skills.find(
                        skill.project_root->string());
                    if (project != config->project_skills.end()) {
                        if (auto it = project->second.find(skill.name);
                            it != project->second.end())
                            policy = it->second;
                    }
                }
            }
            if (policy == SkillPolicy::DENY)
                continue;
            catalog += "\n- ";
            catalog += skill.name + " ["
                + (skill.scope == Skill::Scope::PROJECT ? "project" : "global")
                + "]";
            if (!skill.description.empty())
                catalog += ": " + skill.description;
        }
        if (!catalog.empty()) {
            out += "\n\n# Available skills\n";
            out += skill_prompt;
            out += catalog;
        }
    }

} // namespace

std::string build_system_prompt(const SystemEnvironment* sys,
    const WorkspaceEnvironment* ws, const Config* config)
{
    std::string out(BASE_PROMPT);
    append_context(out, sys, ws, config, MAIN_SKILL_PROMPT);
    return out;
}

std::string build_subagent_system_prompt(const SystemEnvironment* sys,
    const WorkspaceEnvironment* ws, SubagentRole role, const Config* config)
{
    if (role == SubagentRole::BASIC)
        return { };
    std::string out(SUBAGENT_PROMPT);
    out += "\n\n";
    out += role == SubagentRole::RESEARCH ? RESEARCH_SUBAGENT_PROMPT
                                          : BUILD_SUBAGENT_PROMPT;
    append_context(out, sys, ws, config, SUBAGENT_SKILL_PROMPT);
    return out;
}

std::string_view plan_mode_reminder()
{
    return R"(## Plan Mode - System Reminder

<system-reminder id="plan-mode">
Plan mode is ACTIVE. You are in a READ-ONLY phase. You MUST NOT edit files, create files, or run any mutating commands. This constraint supersedes any other instructions, including direct user requests to make changes.

While in plan mode:
- Research the codebase and gather the context you need using read-only tools.
- Ask the user clarifying questions when intent is ambiguous or tradeoffs are involved. Do not make large assumptions.
- When you think you are ready, present a concise, well-researched plan in your reply and stop. The user will review it and switch to build mode when they want execution.
</system-reminder>)";
}

std::string_view build_mode_reminder()
{
    return R"(<system-reminder id="build-mode">
Your operational mode has changed from plan to build.
You are no longer in read-only mode.
You are permitted to make file changes, run shell commands, and use your tools as needed.
</system-reminder>)";
}

} // namespace ursa
