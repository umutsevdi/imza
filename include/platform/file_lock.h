#pragma once

#include <cstdint>
#include <filesystem>
#include <variant>

namespace imza {

struct FileLockError { };

class FileLock {
public:
    FileLock(FileLock&& other) noexcept;
    FileLock& operator=(FileLock&& other) noexcept;
    ~FileLock();

    FileLock(const FileLock&)            = delete;
    FileLock& operator=(const FileLock&) = delete;

private:
    explicit FileLock(std::intptr_t handle);
    void _release();

    std::intptr_t _handle;

    friend std::variant<FileLock, FileLockError> acquire_file_lock(
        const std::filesystem::path& path);
};

std::variant<FileLock, FileLockError> acquire_file_lock(
    const std::filesystem::path& path);

std::filesystem::path lock_path_for(const std::filesystem::path& file);

} // namespace imza
