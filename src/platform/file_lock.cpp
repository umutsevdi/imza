#include "platform/file_lock.h"

#include <system_error>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace imza {

FileLock::FileLock(std::intptr_t handle)
    : _handle(handle)
{
}

FileLock::FileLock(FileLock&& other) noexcept
    : _handle(other._handle)
{
    other._handle = -1;
}

FileLock& FileLock::operator=(FileLock&& other) noexcept
{
    if (this == &other) {
        return *this;
    }
    _release();
    _handle       = other._handle;
    other._handle = -1;
    return *this;
}

FileLock::~FileLock() { _release(); }

void FileLock::_release()
{
    if (_handle == -1) {
        return;
    }
#ifdef _WIN32
    OVERLAPPED overlapped { };
    UnlockFileEx(
        reinterpret_cast<HANDLE>(_handle), 0, MAXDWORD, MAXDWORD, &overlapped);
    CloseHandle(reinterpret_cast<HANDLE>(_handle));
#else
    flock(static_cast<int>(_handle), LOCK_UN);
    close(static_cast<int>(_handle));
#endif
    _handle = -1;
}

std::variant<FileLock, FileLockError> acquire_file_lock(
    const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return FileLockError { };
    }
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return FileLockError { };
    }
    OVERLAPPED overlapped { };
    if (!LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD,
            &overlapped)) {
        CloseHandle(handle);
        return FileLockError { };
    }
    return FileLock { reinterpret_cast<std::intptr_t>(handle) };
#else
    const int handle = open(path.c_str(), O_CREAT | O_RDWR, 0600);
    if (handle < 0) {
        return FileLockError { };
    }
    if (flock(handle, LOCK_EX) != 0) {
        close(handle);
        return FileLockError { };
    }
    return FileLock { handle };
#endif
}

std::filesystem::path lock_path_for(const std::filesystem::path& file)
{
    return file.string() + ".lock";
}

} // namespace imza
