#include "platform/file_lock.h"

#include <system_error>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace imza {

FileLock::FileLock(std::intptr_t handle, std::filesystem::path path)
    : _handle(handle)
    , _path(std::move(path))
{
}

FileLock::FileLock(FileLock&& other) noexcept
    : _handle(other._handle)
    , _path(std::move(other._path))
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
    _path         = std::move(other._path);
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
    ReleaseMutex(reinterpret_cast<HANDLE>(_handle));
    CloseHandle(reinterpret_cast<HANDLE>(_handle));
#else
    struct stat handle_info { };
    struct stat path_info { };
    if (fstat(static_cast<int>(_handle), &handle_info) == 0
        && stat(_path.c_str(), &path_info) == 0
        && handle_info.st_dev == path_info.st_dev
        && handle_info.st_ino == path_info.st_ino) {
        unlink(_path.c_str());
    }
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
    std::uint64_t hash = 1469598103934665603ULL;
    for (const wchar_t value : path.wstring()) {
        hash ^= static_cast<std::uint64_t>(value);
        hash *= 1099511628211ULL;
    }
    const std::wstring name = L"Local\\imza-file-lock-" + std::to_wstring(hash);
    HANDLE handle           = CreateMutexW(nullptr, FALSE, name.c_str());
    if (handle == nullptr) {
        return FileLockError { };
    }
    const DWORD result = WaitForSingleObject(handle, INFINITE);
    if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) {
        CloseHandle(handle);
        return FileLockError { };
    }
    return FileLock { reinterpret_cast<std::intptr_t>(handle), path };
#else
    while (true) {
        const int handle = open(path.c_str(), O_CREAT | O_RDWR, 0600);
        if (handle < 0) {
            return FileLockError { };
        }
        if (flock(handle, LOCK_EX) != 0) {
            close(handle);
            return FileLockError { };
        }

        struct stat handle_info { };
        struct stat path_info { };
        if (fstat(handle, &handle_info) != 0) {
            flock(handle, LOCK_UN);
            close(handle);
            return FileLockError { };
        }
        if (stat(path.c_str(), &path_info) == 0
            && handle_info.st_dev == path_info.st_dev
            && handle_info.st_ino == path_info.st_ino) {
            return FileLock { handle, path };
        }
        flock(handle, LOCK_UN);
        close(handle);
    }
#endif
}

std::filesystem::path lock_path_for(const std::filesystem::path& file)
{
    return file.string() + ".lock";
}

} // namespace imza
