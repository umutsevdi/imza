#pragma once

#include <memory>

namespace imza {

struct ApplicationState;
class MainThreadQueue;

int run_repl(
    std::shared_ptr<ApplicationState> state, MainThreadQueue& main_thread);

} // namespace imza
