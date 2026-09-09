#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

namespace imza {

struct CommandResult {
    std::string output;
    int exit_code  = 0;
    bool timed_out = false;
    bool spawned   = false;
};

std::string shell_quote(const std::filesystem::path& path);
CommandResult run_command(
    const std::string& command, std::chrono::seconds timeout);
CommandResult run_attached_command(const std::string& command);
bool open_browser(std::string_view url);

} // namespace imza
