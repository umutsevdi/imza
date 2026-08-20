#pragma once

#include "types.hpp"

namespace ursa {

// Runs the interactive FTXUI chat interface. Returns the process exit code.
int run_repl(const Config& cfg);

} // namespace ursa
