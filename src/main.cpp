#include <filesystem>
#include <functional>
#include <print>
#include <string>
#include <utility>

#include "app/application_state.h"
#include "app/flows.h"
#include "conversation/persistence.h"
#include "permissions/store.h"
#include "platform/config.h"
#include "runtime/main_thread_queue.h"
#include "ui/repl.h"

int main(int argc, char** argv)
{
    const imza::CliResult cli = imza::run_cli(argc, argv);
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

    const auto path = imza::config_path();
    imza::Config cfg;
    std::string error;
    const auto status = imza::load_config(path, cfg, &error);
    if (status != imza::Status::OK) {
        std::println("config error ({}): {}", static_cast<int>(status),
            error.empty() ? path.string() : error);
        return 1;
    }

    const imza::RuntimeFlag runtime = imza::runtime_flags_for(cli);
    imza::MainThreadQueue main_thread;
    auto state = imza::make_application_state(
        [&main_thread](
            std::function<void()> task) { main_thread.post(std::move(task)); },
        std::move(cfg), imza::StreamFn { }, runtime);
    imza::PermissionStore::Grants startup_grants;
    startup_grants.reserve(
        cli.allowed_directories.size() + cli.allowed_commands.size());
    for (const std::filesystem::path& directory : cli.allowed_directories) {
        startup_grants.emplace_back(directory);
    }
    for (const imza::ShellCommandGrant& command : cli.allowed_commands) {
        startup_grants.emplace_back(command);
    }
    if (!state->permissions->install(std::move(startup_grants))) {
        std::println(stderr, "failed to install startup permission grants");
        return 2;
    }
    if (cli.session_path) {
        std::filesystem::path workspace;
        if (imza::load_session(*cli.session_path, *state->session, &workspace)
                != imza::Status::OK
            || (!cli.working_directory
                && !state->environment->chdir(workspace))) {
            std::println(stderr, "failed to load session '{}'",
                cli.session_path->stem().string());
            return 2;
        }
    }
    if (cli.model) {
        const imza::Config config = state->providers->config();
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
    if (cli.one_shot) {
        const imza::OneShotResult result
            = imza::run_one_shot(*state, main_thread, *cli.one_shot);
        if (!result.output.empty()) {
            std::println("{}", result.output);
        }
        if (!result.error.empty()) {
            std::println(stderr, "{}", result.error);
        }
        return imza::one_shot_exit_code(result.kind);
    }
    return imza::run_repl(std::move(state), main_thread);
}
