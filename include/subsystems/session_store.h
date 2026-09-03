#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/config.h"
#include "common/types.h"

namespace ursa {

class Session;

struct SavedSession {
    std::filesystem::path path;
    std::string title;
    std::string saved_at;
};

Status save_session(Session& session);
Status load_session(const std::filesystem::path& path, Session& session,
    std::filesystem::path* workspace = nullptr);
std::vector<SavedSession> saved_sessions();

enum class DeleteSessionResult { OK, INVALID_PATH, REMOVE_FAILED };
DeleteSessionResult delete_saved_session(const std::filesystem::path& path);

} // namespace ursa
