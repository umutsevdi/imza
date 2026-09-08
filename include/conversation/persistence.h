#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "common/types.h"

namespace ursa {

class Session;
struct LoadedSession;

Status save_session(Session& session);
Status read_session(const std::filesystem::path& path, LoadedSession& loaded);
Status load_session(const std::filesystem::path& path, Session& session,
    std::filesystem::path* workspace = nullptr);
std::vector<SavedSession> saved_sessions();

enum class DeleteSessionResult { OK, INVALID_PATH, REMOVE_FAILED };
DeleteSessionResult delete_saved_session(const std::filesystem::path& path);

} // namespace ursa
