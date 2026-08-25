#include "environment.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
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
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'
                                || out.back() == ' ')) {
        out.pop_back();
    }
    return out;
}

std::string trim(const std::string& s)
{
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end
        && std::isspace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    while (end > begin
        && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(begin, end - begin);
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
    const auto now = std::chrono::system_clock::now();
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

void parse_os_release(Environment& env)
{
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
        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        if (!value.empty() && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        value = trim(value);
        if (key == "NAME") {
            name = value;
        } else if (key == "VERSION_ID") {
            version_id = value;
        } else if (key == "PRETTY_NAME") {
            pretty = value;
        }
    }
    if (!pretty.empty()) {
        env.distro = pretty;
    } else if (!name.empty()) {
        env.distro = name + (version_id.empty() ? "" : " " + version_id);
    }
    env.os_name = "Linux";
    env.os_version = read_command_output("uname -r");
}

#ifdef __APPLE__
void detect_os(Environment& env)
{
    env.os_name = "macOS";
    env.os_version = read_command_output("sw_vers -productVersion");
    env.default_shell = get_env("SHELL");
    if (env.default_shell.empty()) {
        env.default_shell = "/bin/zsh";
    }
}
#elif defined(_WIN32)
void detect_os(Environment& env)
{
    env.os_name = "Windows";
    env.default_shell = get_env("COMSPEC");
    if (env.default_shell.empty()) {
        env.default_shell = "cmd.exe";
    }
    const std::string ver = read_command_output("cmd /c ver");
    const auto open = ver.find('[');
    const auto close = ver.find(']');
    if (open != std::string::npos && close != std::string::npos
        && close > open) {
        std::string inner = trim(ver.substr(open + 1, close - open - 1));
        const auto pos = inner.find("Version ");
        if (pos != std::string::npos) {
            inner = trim(inner.substr(pos + 8));
        }
        env.os_version = inner;
    }
}
#else
void detect_os(Environment& env)
{
    parse_os_release(env);
    env.default_shell = get_env("SHELL");
    if (env.default_shell.empty()) {
        env.default_shell = "/bin/sh";
    }
}
#endif

void detect_package_managers(Environment& env)
{
    static const char* const candidates[] = {
        "apt", "apt-get", "dnf", "yum", "pacman", "zypper", "apk",
        "brew", "port", "xbps-install", "nix-env", "snap", "flatpak",
        "winget", "choco", "scoop"
    };
    for (const char* pm : candidates) {
        if (find_in_path(pm)) {
            env.package_managers.push_back(pm);
        }
    }
}

} // namespace

Environment analyze_environment()
{
    Environment env;
    detect_os(env);
    detect_package_managers(env);
    env.today = today_string();
    return env;
}

std::shared_future<Environment> analyze_environment_async()
{
    return std::async(std::launch::async, analyze_environment).share();
}

} // namespace ursa
