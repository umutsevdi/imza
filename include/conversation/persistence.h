#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "common/types.h"

namespace imza {

class Session;
struct LoadedSession;

// Title shown for sessions saved without a user-set name.
inline constexpr char UNTITLED_TITLE[] = "Untitled session";

Status save_session(Session& session);
Status read_session(const std::filesystem::path& path, LoadedSession& loaded);
std::vector<SavedSession> saved_sessions();
bool session_file_locked(const std::filesystem::path& path);

enum class DeleteSessionResult { OK, INVALID_PATH, REMOVE_FAILED };
DeleteSessionResult delete_saved_session(const std::filesystem::path& path);

} // namespace imza
