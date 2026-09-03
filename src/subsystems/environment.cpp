#include "subsystems/environment.h"

#include "core/command_runner.h"
#include "core/config.h"
#include "common/util.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace ursa {

namespace {

    constexpr std::uint64_t FNV_OFFSET = 14695981039346656037ULL;
    constexpr std::uint64_t FNV_PRIME  = 1099511628211ULL;

    void hash_bytes(std::uint64_t& signature, std::string_view value)
    {
        for (const unsigned char byte : value) {
            signature ^= byte;
            signature *= FNV_PRIME;
        }
    }

    bool has_status(std::string_view status, char value)
    {
        return status.find(value) != std::string_view::npos;
    }

    ChangedFile::Kind changed_file_kind(std::string_view status)
    {
        if (has_status(status, 'U') || status == "AA" || status == "DD") {
            return ChangedFile::Kind::CONFLICTED;
        }
        if (has_status(status, 'D')) {
            return ChangedFile::Kind::DELETED;
        }
        if (has_status(status, 'R')) {
            return ChangedFile::Kind::RENAMED;
        }
        if (has_status(status, 'C')) {
            return ChangedFile::Kind::COPIED;
        }
        if (status == "??") {
            return ChangedFile::Kind::UNTRACKED;
        }
        if (has_status(status, 'A')) {
            return ChangedFile::Kind::ADDED;
        }
        if (has_status(status, 'M') || has_status(status, 'T')) {
            return ChangedFile::Kind::MODIFIED;
        }
        return ChangedFile::Kind::UNKNOWN;
    }

    void append_untracked_summary(const std::filesystem::path& root,
        const std::vector<ChangedFile>& files, ChangeSummary& summary)
    {
        for (const ChangedFile& file : files) {
            if (file.kind != ChangedFile::Kind::UNTRACKED) {
                continue;
            }
            hash_bytes(summary.signature, file.path);
            summary.signature ^= 0xFF;
            summary.signature *= FNV_PRIME;
            std::ifstream input(root / file.path, std::ios::binary);
            if (!input) {
                continue;
            }
            std::array<char, 8192> buffer;
            std::size_t lines = 0;
            bool binary       = false;
            bool has_content  = false;
            char last         = '\0';
            while (input) {
                input.read(buffer.data(), buffer.size());
                const std::streamsize count = input.gcount();
                if (count <= 0) {
                    continue;
                }
                const std::string_view chunk(
                    buffer.data(), static_cast<std::size_t>(count));
                hash_bytes(summary.signature, chunk);
                has_content = true;
                last        = chunk.back();
                binary = binary || chunk.find('\0') != std::string_view::npos;
                lines += std::ranges::count(chunk, '\n');
            }
            if (!binary) {
                if (has_content && last != '\n') {
                    ++lines;
                }
                summary.additions += lines;
            }
        }
    }

    std::string read_command_output(const std::string& cmd)
    {
#ifdef _WIN32
        FILE* pipe = _popen(cmd.c_str(), "r");
#else
        FILE* pipe = popen(cmd.c_str(), "r");
#endif
        if (pipe == nullptr) {
            return "";
        }
        std::string out;
        char buf[512];
        while (std::fgets(buf, sizeof(buf), pipe) != nullptr) {
            out += buf;
        }
#ifdef _WIN32
        _pclose(pipe);
#else
        pclose(pipe);
#endif
        while (!out.empty()
            && (out.back() == '\n' || out.back() == '\r'
                || out.back() == ' ')) {
            out.pop_back();
        }
        return out;
    }

    bool find_in_path(const std::string& name)
    {
#ifdef _WIN32
        return !read_command_output("where " + name).empty();
#else
        return !read_command_output("command -v " + name).empty();
#endif
    }

    void detect_os(std::string& os_name, std::string& os_version,
        std::string& default_shell)
    {
#ifdef __APPLE__
        os_name       = "macOS";
        os_version    = read_command_output("sw_vers -productVersion");
        default_shell = env_or_empty("SHELL");
        if (default_shell.empty()) {
            default_shell = "/bin/zsh";
        }

#elif defined(_WIN32)
        os_name       = "Windows";
        default_shell = env_or_empty("COMSPEC");
        if (default_shell.empty()) {
            default_shell = "cmd.exe";
        }
        const std::string ver = read_command_output("cmd /c ver");
        const auto open       = ver.find('[');
        const auto close      = ver.find(']');
        if (open != std::string::npos && close != std::string::npos
            && close > open) {
            std::string inner = trim(ver.substr(open + 1, close - open - 1));
            const auto pos    = inner.find("Version ");
            if (pos != std::string::npos) {
                inner = trim(inner.substr(pos + 8));
            }
            os_version = inner;
        }
#else
        std::ifstream in("/etc/os-release");
        if (!in) {
            in.open("/etc/lsb-release");
        }
        std::string name;
        std::string version_id;
        std::string pretty;
        std::string line;
        while (std::getline(in, line)) {
            const auto eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            std::string key   = line.substr(0, eq);
            std::string value = line.substr(eq + 1);
            if (!value.empty() && value.front() == '"' && value.back() == '"') {
                value = value.substr(1, value.size() - 2);
            }
            value = std::string(trim(value));
            if (key == "NAME") {
                name = value;
            } else if (key == "VERSION_ID") {
                version_id = value;
            } else if (key == "PRETTY_NAME") {
                pretty = value;
            }
        }
        if (!pretty.empty()) {
            os_name = pretty;
        } else if (!name.empty()) {
            os_name = name + (version_id.empty() ? "" : " " + version_id);
        } else {
            os_name = "Linux";
        }
        os_version    = read_command_output("uname -r");
        default_shell = env_or_empty("SHELL");
        if (default_shell.empty()) {
            default_shell = "/bin/sh";
        }
#endif
    }

    void detect_package_managers(std::vector<std::string>& package_managers)
    {
        static const char* const candidates[] = { "apt", "apt-get", "dnf",
            "yum", "pacman", "zypper", "apk", "brew", "port", "xbps-install",
            "nix-env", "snap", "flatpak", "winget", "choco", "scoop" };
        for (const char* pm : candidates) {
            if (find_in_path(pm)) {
                package_managers.emplace_back(pm);
            }
        }
    }

    std::string skill_description(const std::filesystem::path& path)
    {
        std::ifstream in(path);
        std::string line;
        bool frontmatter = false;
        while (std::getline(in, line)) {
            if (line == "---") {
                if (frontmatter) {
                    break;
                }
                frontmatter = true;
                continue;
            }
            if (frontmatter && line.starts_with("description:")) {
                std::string value
                    = std::string(trim(std::string_view(line).substr(12)));
                if (value.size() >= 2 && value.front() == '"'
                    && value.back() == '"') {
                    value = value.substr(1, value.size() - 2);
                }
                return value;
            }
        }
        return { };
    }

    void add_skills(const std::filesystem::path& directory, Skill::Scope scope,
        const std::optional<std::filesystem::path>& root,
        std::unordered_map<std::string, Skill>& skills)
    {
        std::error_code ec;
        if (!std::filesystem::is_directory(directory, ec)) {
            return;
        }
        for (std::filesystem::directory_iterator it(directory, ec), end;
            !ec && it != end; it.increment(ec)) {
            const auto file = it->path() / "SKILL.md";
            if (!it->is_directory(ec)
                || !std::filesystem::is_regular_file(file, ec)) {
                continue;
            }
            const std::string name = it->path().filename().string();
            skills.emplace(name,
                Skill { name, skill_description(file), file, scope, root });
        }
    }

    void detect_global_skills(
        std::unordered_map<std::string, Skill>& global_skills)
    {
        auto config_dir            = base_config_dir();
        std::filesystem::path home = { home_dir() };
        auto home_dir_skills = { ".opencode", ".claude", ".codex", ".grok",
            ".gemini", ".agents", ".cursor", ".openclaw" };
        for (const auto& skill_path : home_dir_skills) {
            auto p = home / skill_path / "skills";
            add_skills(p, Skill::Scope::GLOBAL, std::nullopt, global_skills);
        }
        auto skills_generic_cfg
            = config_dir.parent_path() / "agents" / "skills";
        add_skills(skills_generic_cfg, Skill::Scope::GLOBAL, std::nullopt,
            global_skills);
    }

    void detect_project_skills(const std::filesystem::path& root,
        std::unordered_map<std::string, Skill>& project_skills)
    {
        auto home_dir_skills = { ".opencode", ".claude", ".codex", ".grok",
            ".gemini", ".agents", ".cursor", ".openclaw" };
        for (const auto& skill_path : home_dir_skills) {
            auto p = root / skill_path / "skills";
            add_skills(p, Skill::Scope::PROJECT, root, project_skills);
        }
    }

} // namespace

std::vector<ChangedFile> parse_git_status(std::string_view status)
{
    std::vector<ChangedFile> files;
    for (std::string line : split_lines(status)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.size() < 4 || line[2] != ' ') {
            continue;
        }
        files.push_back(ChangedFile { line.substr(3),
            changed_file_kind(std::string_view(line).substr(0, 2)) });
    }
    return files;
}

std::string normalize_git_branch(std::string_view branch)
{
    return std::string(trim(branch));
}

ChangeSummary summarize_git_diff(std::string_view diff)
{
    ChangeSummary summary;
    summary.signature = FNV_OFFSET;
    hash_bytes(summary.signature, diff);
    std::size_t line_start = 0;
    while (line_start < diff.size()) {
        const std::size_t line_end  = diff.find('\n', line_start);
        const std::string_view line = diff.substr(line_start,
            line_end == std::string_view::npos ? diff.size() - line_start
                                               : line_end - line_start);
        line_start
            = line_end == std::string_view::npos ? diff.size() : line_end + 1;
        if (line.empty()) {
            break;
        }
        const std::size_t first_tab = line.find('\t');
        if (first_tab == std::string_view::npos) {
            break;
        }
        const std::size_t second_tab = line.find('\t', first_tab + 1);
        if (second_tab == std::string_view::npos) {
            break;
        }
        std::size_t additions        = 0;
        std::size_t deletions        = 0;
        const std::string_view added = line.substr(0, first_tab);
        const std::string_view deleted
            = line.substr(first_tab + 1, second_tab - first_tab - 1);
        const auto added_result = std::from_chars(
            added.data(), added.data() + added.size(), additions);
        const auto deleted_result = std::from_chars(
            deleted.data(), deleted.data() + deleted.size(), deletions);
        if (added_result.ec == std::errc { }
            && deleted_result.ec == std::errc { }) {
            summary.additions += additions;
            summary.deletions += deletions;
        }
    }
    return summary;
}

std::optional<InstructionFile> load_agent_file(
    const std::filesystem::path& root)
{
    constexpr std::size_t max_bytes = 32 * 1024;
    static const char* const candidates[]
        = { "AGENTS.md", "CLAUDE.md", "GEMINI.md" };
    for (const char* name : candidates) {
        const std::filesystem::path path = root / name;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec) || ec) {
            continue;
        }
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            continue;
        }
        std::string content { std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>() };
        if (content.empty()) {
            continue;
        }
        if (content.size() > max_bytes) {
            content.resize(max_bytes);
            content += "\n[truncated]";
        }
        return InstructionFile { name, std::move(content) };
    }
    return std::nullopt;
}

SystemEnvironment::SystemEnvironment()
{
    detect_os(os_name, os_version, default_shell);
    detect_package_managers(package_managers);
    detect_global_skills(global_skills);
    has_git = find_in_path("git");
    today   = format_local_time("%Y-%m-%d");
}

WorkspaceEnvironment::WorkspaceEnvironment(const std::filesystem::path& dir)
{
    std::error_code ec;
    auto p = std::filesystem::path { dir };
    while (true) {
        if (std::filesystem::exists(p / ".git", ec)) {
            if (!ec) {
                project_root = p;
            }
            break;
        }
        if (p.parent_path() == p) {
            break;
        }
        p = p.parent_path();
    }
    if (project_root.has_value()) {
        instruction = load_agent_file(project_root.value());
        detect_project_skills(project_root.value(), project_skills);
    }
}

Environment::Environment()
    : system_(std::make_shared<const SystemEnvironment>())
{
    worker_ = std::jthread([this] {
        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::current_path(ec);
        auto workspace = std::make_shared<WorkspaceEnvironment>(dir);
        _publish_workspace(workspace->project_root.has_value()
                ? std::move(workspace)
                : nullptr,
            0);
    });

    if (system_->has_git) {
        git_worker_ = std::jthread([this](const std::stop_token& stop) {
            {
                std::unique_lock lock(workspace_mutex_);
                if (!workspace_ready_cv_.wait(
                        lock, stop, [this] { return ready_.load(); })) {
                    return;
                }
            }
            while (!stop.stop_requested()) {
                const auto observed_workspace = workspace();
                if (observed_workspace == nullptr) {
                    std::this_thread::sleep_for(std::chrono::seconds { 2 });
                    continue;
                }
                const CommandResult status = run_command(
                    "git status --porcelain=v1 --untracked-files=all",
                    std::chrono::seconds { 1 });
                const CommandResult branch = run_command(
                    "git branch --show-current", std::chrono::seconds { 1 });
                CommandResult diff
                    = run_command("git diff --no-ext-diff --no-color --numstat "
                                  "--patch HEAD --",
                        std::chrono::seconds { 10 });
                if (diff.spawned && !diff.timed_out && diff.exit_code != 0) {
                    diff = run_command("git diff --cached --no-ext-diff "
                                       "--no-color --numstat --patch --",
                        std::chrono::seconds { 10 });
                }
                if (status.spawned && !status.timed_out && status.exit_code == 0
                    && branch.spawned && !branch.timed_out
                    && branch.exit_code == 0 && diff.spawned && !diff.timed_out
                    && diff.exit_code == 0) {
                    std::vector<ChangedFile> changed_files
                        = parse_git_status(status.output);
                    const std::string branch_name
                        = normalize_git_branch(branch.output);
                    ChangeSummary changes = summarize_git_diff(diff.output);
                    append_untracked_summary(
                        observed_workspace->project_root.value(), changed_files,
                        changes);
                    const auto current = repository();
                    if (current && changed_files == current->changed_files
                        && branch_name == current->branch
                        && changes == current->changes) {
                        std::this_thread::sleep_for(std::chrono::seconds { 2 });
                        continue;
                    }
                    RepositoryState next;
                    next.changed_files = std::move(changed_files);
                    next.branch        = branch_name;
                    next.changes       = changes;
                    _publish_repository(
                        std::make_shared<RepositoryState>(std::move(next)),
                        observed_workspace);
                }
                std::this_thread::sleep_for(std::chrono::seconds { 2 });
            }
        });
    }
}

void Environment::_publish_workspace(
    std::shared_ptr<const WorkspaceEnvironment> ws, std::uint64_t generation)
{
    {
        std::unique_lock lock(workspace_mutex_);
        if (generation != workspace_generation_) {
            return;
        }
        workspace_  = std::move(ws);
        repository_ = std::make_shared<const RepositoryState>();
        ready_.store(true);
    }
    workspace_ready_cv_.notify_all();
    workspace_changed_.publish();
    repository_changed_.publish();
}

void Environment::_publish_repository(
    std::shared_ptr<const RepositoryState> repository,
    const std::shared_ptr<const WorkspaceEnvironment>& workspace)
{
    {
        std::unique_lock lock(workspace_mutex_);
        if (workspace_ != workspace) {
            return;
        }
        repository_ = std::move(repository);
    }
    repository_changed_.publish();
}

bool Environment::chdir(const std::filesystem::path& dir)
{
    std::error_code ec;
    std::filesystem::current_path(dir, ec);
    if (ec) {
        return false;
    }
    std::uint64_t generation;
    {
        std::unique_lock lock(workspace_mutex_);
        generation = ++workspace_generation_;
    }
    auto workspace = std::make_shared<WorkspaceEnvironment>(dir);
    _publish_workspace(
        workspace->project_root.has_value() ? std::move(workspace) : nullptr,
        generation);
    return true;
}

Signal<>::Subscription Environment::subscribe_to_workspace_change(
    Signal<>::Callback callback)
{
    Signal<>::Subscription subscription;
    bool notify_now;
    {
        std::unique_lock lock(workspace_mutex_);
        subscription = workspace_changed_.subscribe(callback);
        notify_now   = ready_.load();
    }
    if (notify_now) {
        callback();
    }
    return subscription;
}

Signal<>::Subscription Environment::subscribe_to_repository_change(
    Signal<>::Callback callback)
{
    Signal<>::Subscription subscription;
    bool notify_now;
    {
        std::unique_lock lock(workspace_mutex_);
        subscription = repository_changed_.subscribe(callback);
        notify_now   = repository_ != nullptr;
    }
    if (notify_now) {
        callback();
    }
    return subscription;
}

std::optional<std::string> Environment::agent_rules_path() const
{
    const auto ws = workspace();
    if (ws == nullptr || !ws->instruction) {
        return std::nullopt;
    }
    return ws->instruction->path;
}

std::vector<Skill> Environment::skills() const
{
    std::vector<Skill> out;
    for (const auto& [name, skill] : system_->global_skills) {
        out.push_back(skill);
    }
    const auto ws = workspace();
    if (ws) {
        for (const auto& [name, skill] : ws->project_skills) {
            out.push_back(skill);
        }
    }
    std::sort(out.begin(), out.end(), [](const Skill& a, const Skill& b) {
        if (a.scope != b.scope) {
            return a.scope == Skill::Scope::PROJECT;
        }
        return a.name < b.name;
    });
    return out;
}

} // namespace ursa
