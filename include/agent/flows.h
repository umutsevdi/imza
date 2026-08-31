#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "agent/application_state.h"

namespace ursa {

void submit(ApplicationState& state, std::string text,
    std::vector<FileAttachment> attachments = { });
void resolve_modal(ApplicationState& state, ModalResult result);
void close_modal(ApplicationState& state);
void enqueue_user_modal(ApplicationState& state, ModalPayload payload);
std::future<ModalResult> request_modal(ApplicationState& state,
    ModalPayload payload);
void present_front(ApplicationState& state);
void drain_queued(ApplicationState& state);
void on_turn_finished(ApplicationState& state, std::string error);
void run_slash(ApplicationState& state, std::string_view command);
void interrupt(ApplicationState& state);
void delete_saved_session(ApplicationState& state,
    const std::filesystem::path& path);

} // namespace ursa
