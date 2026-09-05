#include <cstdio>
#include <filesystem>
#include <functional>
#include <print>
#include <string>
#include <utility>

#include "agent/application_state.h"
#include "agent/flows.h"
#include "agent/tools.h"
#include "core/config.h"
#include "subsystems/main_thread_queue.h"
#include "subsystems/session_store.h"
#include "ui/repl.h"

int main(int argc, char** argv)
{
    const ursa::CliResult cli = ursa::run_cli(argc, argv);
    if (!cli.continue_as_interactive) {
        return cli.exit_code;
    }
    if (cli.working_directory) {
        std::error_code error;
        std::filesystem::current_path(*cli.working_directory, error);
        if (error) {
            std::println(stderr, "failed to open directory '{}': {}",
                cli.working_directory->string(), error.message());
            return 2;
        }
    }

    const auto path = ursa::config_path();
    ursa::Config cfg;
    std::string error;
    const auto status = ursa::load_config(path, cfg, &error);
    if (status != ursa::Status::OK) {
        std::println("config error ({}): {}", static_cast<int>(status),
            error.empty() ? path.string() : error);
        return 1;
    }

    const bool web_enabled   = cli.web.value_or(true);
    const bool shell_enabled = cli.shell.value_or(true);
    ursa::MainThreadQueue main_thread;
    auto state = ursa::make_application_state(
        [&main_thread](
            std::function<void()> task) { main_thread.post(std::move(task)); },
        std::move(cfg), ursa::StreamFn { },
        ursa::default_tools(true, web_enabled, shell_enabled));
    state->web_enabled   = web_enabled;
    state->shell_enabled = shell_enabled;
    if (cli.session_path) {
        std::filesystem::path workspace;
        if (ursa::load_session(*cli.session_path, *state->session, &workspace)
                != ursa::Status::OK
            || (!cli.working_directory
                && !state->environment->chdir(workspace))) {
            std::println(stderr, "failed to load session '{}'",
                cli.session_path->stem().string());
            return 2;
        }
    }
    if (cli.model) {
        const ursa::Config config = state->providers->config();
        std::string connection_id;
        if (config.last_used) {
            connection_id = config.last_used->provider;
        } else if (config.providers.size() == 1) {
            connection_id = config.providers.front().id;
        }
        if (connection_id.empty()
            || !state->providers->select_model(
                { std::move(connection_id), *cli.model })) {
            std::println(stderr,
                "failed to select model '{}'; select a provider first",
                *cli.model);
            return 2;
        }
    }
    if (cli.variant && !state->providers->set_reasoning_effort(*cli.variant)) {
        std::println(stderr, "failed to select variant '{}'", *cli.variant);
        return 2;
    }
    return ursa::run_repl(std::move(state), main_thread);
}
