#pragma once

#include <chrono>
#include <string>

namespace ursa {

struct CommandResult {
    std::string output;
    int exit_code  = 0;
    bool timed_out = false;
    bool spawned   = false;
};

CommandResult run_command(
    const std::string& command, std::chrono::seconds timeout);

} // namespace ursa
