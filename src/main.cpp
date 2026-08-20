#include <print>

#include "types.hpp"
#include "ui.hpp"

int main()
{
    const auto path = ursa::config_path();
    ursa::Config cfg;
    const auto status = ursa::load_config(path, cfg);
    if (status != ursa::Status::OK) {
        std::println(
            "config error ({}) at {}", static_cast<int>(status), path.string());
        return 1;
    }

    return ursa::run_repl(cfg);
}
