#include <chrono>
#include <filesystem>
#include <functional>
#include <print>
#include <string>
#include <thread>
#include <utility>

#include "app/application_state.h"
#include "app/flows.h"
#include "conversation/persistence.h"
#include "permissions/permission_store.h"
#include "platform/config.h"
#include "platform/console_mode.h"
#include "platform/credential_store.h"
#include "runtime/main_thread_queue.h"
#include "runtime/mcp_subsystem.h"
#include "tools/skills.h"
#include "ui/ui.h"

int main(int argc, char** argv)
{
#ifdef _WIN32
    const imza::ConsoleModeScope console_mode;
#endif
    const imza::CliResult cli = imza::run_cli(argc, argv);
    if (!cli.continue_as_interactive) {
        if (cli.update_requested) {
            return imza::run_update_display(imza::run_update_command);
        }
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

    // A failed probe degrades to plaintext for this run; the notice is
    // published once the state exists.
    auto credentials = std::make_shared<imza::CredentialStore>(
        cfg.preferences.credentials == imza::Preferences::Credentials::PLAINTEXT
            ? imza::CredentialStore::Mode::PLAINTEXT
            : imza::CredentialStore::Mode::OS);
    if (imza::resolve_credentials(cfg, *credentials) != imza::Status::OK) {
        std::println("failed to read stored credentials");
        return 1;
    }
    imza::adopt_legacy_credentials(path, cfg, *credentials);

    const imza::RuntimeFlag runtime = imza::runtime_flags_for(cli);
    // Read before the config is moved into the application state.
    imza::PermissionStore::Grants startup_grants
        = imza::allow_grants(cfg.allow);
    startup_grants.reserve(startup_grants.size()
        + cli.allowed_directories.size() + cli.allowed_commands.size()
        + cli.allowed_mcp.size() + cli.allowed_skills.size());
    const bool keychain_available      = credentials->is_keychain_available();
    const std::string plaintext_reason = credentials->unavailable_reason();
    auto state = imza::make_application_state(imza::PostFn { }, std::move(cfg),
        imza::StreamFn { }, runtime, std::move(credentials));
    if (!keychain_available && !plaintext_reason.empty()) {
        state->notifications->publish(true,
            "Credentials will be stored as plaintext because "
                + plaintext_reason + ".");
    }
    for (const std::filesystem::path& directory : cli.allowed_directories) {
        startup_grants.emplace_back(directory);
    }
    for (const imza::ShellCommandGrant& command : cli.allowed_commands) {
        startup_grants.emplace_back(command);
    }
    // State does not matter here: the grant covers the connect too.
    for (const std::string& id : cli.allowed_mcp) {
        bool known = false;
        for (const imza::McpServerSnapshot& server : state->mcp->snapshot()) {
            if (server.id == id) {
                known = true;
                break;
            }
        }
        if (!known) {
            std::println(stderr, "cannot allow '{}': no such MCP server", id);
            return 2;
        }
        startup_grants.emplace_back(imza::McpGrant { id });
    }
    // Resolve names the way the skill tool does: project wins over global.
    // The catalogue lands on an environment worker, so wait for it first.
    // Only --allow-skill reads it during startup; one-shot runs wait on
    // their own in run_one_shot, so interactive startup never blocks here.
    if (!cli.allowed_skills.empty()) {
        for (int waited = 0; waited < 5000 && !state->environment->ready();
            waited += 50) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    const std::vector<imza::Skill> catalog = state->environment->skills();
    for (const std::string& name : cli.allowed_skills) {
        const imza::Skill* found = nullptr;
        for (const imza::Skill& skill : catalog) {
            if (skill.name != name) {
                continue;
            }
            found = &skill;
            if (skill.scope == imza::Skill::Scope::PROJECT) {
                break;
            }
        }
        if (found == nullptr) {
            std::println(stderr, "cannot allow '{}': no such skill", name);
            return 2;
        }
        startup_grants.emplace_back(
            imza::SkillGrant { *imza::canonical_skill_path(*found) });
    }
    if (!state->permissions->install(std::move(startup_grants))) {
        std::println(stderr, "failed to install startup permission grants");
        return 2;
    }
    if (cli.session_path) {
        // A --cd flag wins: the file still loads, but its recorded
        // workspace does not override the requested directory.
        if (imza::restore_session_at_startup(*state, *cli.session_path,
                cli.working_directory ? std::optional<std::filesystem::path> { }
                                      : std::nullopt)) {
            return imza::run_repl(std::move(state));
        }
        std::println(stderr, "failed to load session '{}': {}",
            cli.session_path->stem().string(),
            state->sessions->is_locked(*cli.session_path)
                ? "session is open in another imza process"
                : "unreadable or invalid session file");
        return 2;
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
            = imza::run_one_shot(*state, *cli.one_shot);
        if (!result.output.empty()) {
            imza::print_markdown_output(result.output);
        }
        if (!result.error.empty()) {
            std::println(stderr, "{}", result.error);
        }
        return imza::one_shot_exit_code(result.kind);
    }
    return imza::run_repl(std::move(state));
}
