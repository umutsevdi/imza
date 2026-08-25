#include <print>

#include "types.h"
#include "ui.h"

int main()
{
    const auto path = ursa::config_path();
    ursa::Config cfg;
    std::string error;
    const auto status = ursa::load_config(path, cfg, &error);
    if (status != ursa::Status::OK) {
        std::println("config error ({}): {}",
            static_cast<int>(status),
            error.empty() ? path.string() : error);
        return 1;
    }

    return ursa::run_repl(cfg);
}
