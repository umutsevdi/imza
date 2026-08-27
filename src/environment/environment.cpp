#include "environment.h"

#include "types.h"
#include "util.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace ursa {

namespace {

    std::string get_env(const char* key)
    {
#ifdef _WIN32
        char* buf = nullptr;
        size_t sz = 0;
        if (_dupenv_s(&buf, &sz, key) != 0 || buf == nullptr) {
            return "";
        }
        std::string value(buf);
        free(buf);
        return value;
#else
        const char* value = std::getenv(key);
        return value != nullptr ? std::string(value) : "";
#endif
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

    std::string today_string()
    {
        const auto now      = std::chrono::system_clock::now();
        const std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm { };
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        std::ostringstream os;
        os << std::put_time(&tm, "%Y-%m-%d");
        return os.str();
    }

    void detect_os(std::string& os_name, std::string& os_version,
        std::string& default_shell)
    {
#ifdef __APPLE__
        os_name       = "macOS";
        os_version    = read_command_output("sw_vers -productVersion");
        default_shell = get_env("SHELL");
        if (default_shell.empty()) {
            default_shell = "/bin/zsh";
        }

#elif defined(_WIN32)
        os_name       = "Windows";
        default_shell = get_env("COMSPEC");
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
        default_shell = get_env("SHELL");
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
                package_managers.push_back(pm);
            }
        }
    }

    void detect_global_skills(
        std::unordered_map<std::string, std::filesystem::path>& global_skills)
    {
        auto config_dir            = base_config_dir();
        std::filesystem::path home = { home_dir() };
        auto home_dir_skills = { ".opencode", ".claude", ".codex", ".grok",
            ".gemini", ".agents", ".cursor", ".openclaw" };
        for (const auto& skill_path : home_dir_skills) {
            auto p = home / skill_path / "skills";
            if (std::filesystem::exists(p)) {
                for (auto& skill : std::filesystem::directory_iterator(p)) {
                    if (skill.is_directory()
                        && std::filesystem::exists(skill.path() / "SKILL.md")) {
                        global_skills.emplace(skill.path().filename().string(),
                            skill.path() / "SKILL.md");
                    }
                }
            }
        }
        auto skills_generic_cfg
            = config_dir.parent_path() / "agents" / "skills";
        if (std::filesystem::exists(skills_generic_cfg)) {
            for (auto& skill :
                std::filesystem::directory_iterator(skills_generic_cfg)) {
                if (skill.is_directory()
                    && std::filesystem::exists(skill.path() / "SKILL.md")) {
                    global_skills.emplace(skill.path().filename().string(),
                        skill.path() / "SKILL.md");
                }
            }
        }
    }

    void detect_project_skills(const std::filesystem::path& root,
        std::unordered_map<std::string, std::filesystem::path>& project_skills)
    {
        auto home_dir_skills = { ".opencode", ".claude", ".codex", ".grok",
            ".gemini", ".agents", ".cursor", ".openclaw" };
        for (const auto& skill_path : home_dir_skills) {
            auto p = root / skill_path / "skills";
            if (std::filesystem::exists(p)) {
                for (auto& skill : std::filesystem::directory_iterator(p)) {
                    if (skill.is_directory()
                        && std::filesystem::exists(skill.path() / "SKILL.md")) {
                        project_skills.emplace(skill.path().filename().string(),
                            skill.path() / "SKILL.md");
                    }
                }
            }
        }
    }

} // namespace

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
    today   = today_string();
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
        auto ws = std::make_shared<const WorkspaceEnvironment>(dir);
        publish(std::move(ws));
    });
}

void Environment::publish(std::shared_ptr<const WorkspaceEnvironment> ws)
{
    std::vector<Subscriber> callbacks;
    {
        std::unique_lock lock(workspace_mutex_);
        workspace_ = std::move(ws);
        ready_.store(true);
        callbacks = cbs_;
    }
    for (const auto& sub : callbacks) {
        sub.cb(workspace_);
    }
}

bool Environment::chdir(const std::filesystem::path& dir)
{
    std::error_code ec;
    std::filesystem::current_path(dir, ec);
    if (ec) {
        return false;
    }
    auto next = std::make_shared<const WorkspaceEnvironment>(dir);
    publish(std::move(next));
    return true;
}

std::function<void()> Environment::subscribe_to_workspace_change(
    const std::function<void(std::shared_ptr<const WorkspaceEnvironment>)>& cb)
{
    bool already;
    std::shared_ptr<const WorkspaceEnvironment> current;
    std::uint64_t id;
    {
        std::unique_lock lock(workspace_mutex_);
        already = ready_.load();
        id      = next_id_++;
        cbs_.push_back(Subscriber { id, cb });
        if (already) {
            current = workspace_;
        }
    }
    if (already) {
        cb(current);
    }
    return [this, id] {
        std::unique_lock lock(workspace_mutex_);
        cbs_.erase(std::remove_if(cbs_.begin(), cbs_.end(),
                        [id](const Subscriber& s) { return s.id == id; }),
            cbs_.end());
    };
}

std::optional<std::string> Environment::agent_rules_path() const
{
    const auto ws = workspace();
    if (ws == nullptr || !ws->instruction) {
        return std::nullopt;
    }
    return ws->instruction->path;
}

std::size_t Environment::project_skills() const
{
    const auto ws = workspace();
    return ws == nullptr ? 0 : ws->project_skills.size();
}

std::size_t Environment::global_skills() const
{
    return system_->global_skills.size();
}

std::string shell_name(const SystemEnvironment& sys)
{
    if (sys.default_shell.empty()) {
        return "sh";
    }
    return std::filesystem::path(sys.default_shell).filename().string();
}

std::shared_ptr<Environment> get_environment()
{
    static const std::shared_ptr<Environment> env
        = std::make_shared<Environment>();
    return env;
}

} // namespace ursa
