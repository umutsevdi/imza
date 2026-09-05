#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/application_state.h"

namespace ursa {

struct CliResult {
    bool continue_as_interactive = true;
    int exit_code                = 0;
    std::optional<std::filesystem::path> working_directory;
    std::optional<std::filesystem::path> session_path;
    std::optional<std::string> model;
    std::optional<std::string> variant;
    std::optional<bool> web;
    std::optional<bool> shell;
};

CliResult run_cli(int argc, char** argv);
void submit(ApplicationState& state, std::string text,
    std::vector<FileAttachment> attachments = { });
void resolve_modal(ApplicationState& state, ModalResult result);
void close_modal(ApplicationState& state);
void enqueue_user_modal(ApplicationState& state, ModalPayload payload);
std::future<ModalResult> request_modal(
    ApplicationState& state, ModalPayload payload);
void present_front(ApplicationState& state);
void drain_queued(ApplicationState& state);
void on_turn_finished(ApplicationState& state, std::string error);
void run_slash(ApplicationState& state, std::string_view command);
void interrupt(ApplicationState& state);
void delete_saved_session(
    ApplicationState& state, const std::filesystem::path& path);

} // namespace ursa
