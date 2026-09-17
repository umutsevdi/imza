#pragma once

#include <cstdint>
#include <filesystem>
#include <variant>

namespace imza {

struct FileLockError { };

enum class FileLockMode { SHARED, EXCLUSIVE };

struct FileLockRequest {
    FileLockMode mode = FileLockMode::EXCLUSIVE;
    bool blocking     = true;
};

class FileLock {
public:
    FileLock(FileLock&& other) noexcept;
    FileLock& operator=(FileLock&& other) noexcept;
    ~FileLock();

    FileLock(const FileLock&)            = delete;
    FileLock& operator=(const FileLock&) = delete;

private:
    FileLock(std::intptr_t handle, std::filesystem::path path, bool exclusive);
    void _release();

public:
    const std::filesystem::path& path() const { return _path; }

private:
    std::intptr_t _handle;
    std::filesystem::path _path;
    bool _exclusive = true;

    friend std::variant<FileLock, FileLockError> acquire_file_lock(
        const std::filesystem::path& path, const FileLockRequest& request);
};

// Advisory cross-process lock. Exclusive mode admits one holder; shared mode
// admits any number of holders unless an exclusive lock is live. On Windows
// shared requests degrade to exclusive. A non-blocking request fails with
// FileLockError instead of waiting.
std::variant<FileLock, FileLockError> acquire_file_lock(
    const std::filesystem::path& path, const FileLockRequest& request = { });

// True when any holder - in this process or elsewhere - keeps the file
// locked right now. Never creates a lingering lock file.
bool file_lock_held(const std::filesystem::path& path);

std::filesystem::path lock_path_for(const std::filesystem::path& file);

} // namespace imza
