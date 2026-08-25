#pragma once

#include <chrono>
#include <future>
#include <string>
#include <vector>

namespace ursa {

struct Environment {
    std::string os_name;
    std::string os_version;
    std::string distro;
    std::string default_shell;
    std::vector<std::string> package_managers;
    std::string today;
};

Environment analyze_environment();
std::shared_future<Environment> analyze_environment_async();

} // namespace ursa
