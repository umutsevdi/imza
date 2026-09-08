#pragma once

#include <memory>

namespace ursa {

struct ApplicationState;
class MainThreadQueue;

int run_repl(
    std::shared_ptr<ApplicationState> state, MainThreadQueue& main_thread);

} // namespace ursa
