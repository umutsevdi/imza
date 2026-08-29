#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "types.h"

namespace ursa {

class Session;

struct SavedSession {
    std::filesystem::path path;
    std::string title;
    std::string saved_at;
};

std::filesystem::path data_dir();
std::filesystem::path sessions_dir();
Status save_session(const Session& session);
Status load_session(const std::filesystem::path& path, Session& session);
std::vector<SavedSession> saved_sessions();

}
