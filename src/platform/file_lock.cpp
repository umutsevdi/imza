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

namespace {

#ifdef _WIN32
    std::wstring mutex_name(const std::filesystem::path& path)
    {
        std::uint64_t hash = 1469598103934665603ULL;
        for (const wchar_t value : path.wstring()) {
            hash ^= static_cast<std::uint64_t>(value);
            hash *= 1099511628211ULL;
        }
        return L"Local\\imza-file-lock-" + std::to_wstring(hash);
    }
#else
    int flock_operation(const FileLockRequest& request)
    {
        int operation
            = request.mode == FileLockMode::SHARED ? LOCK_SH : LOCK_EX;
        if (!request.blocking) {
            operation |= LOCK_NB;
        }
        return operation;
    }
#endif

} // namespace

FileLock::FileLock(
    std::intptr_t handle, std::filesystem::path path, const bool exclusive)
    : _handle(handle)
    , _path(std::move(path))
    , _exclusive(exclusive)
{
}

FileLock::FileLock(FileLock&& other) noexcept
    : _handle(other._handle)
    , _path(std::move(other._path))
    , _exclusive(other._exclusive)
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
    _exclusive    = other._exclusive;
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
    // The lock file is only unlinked when this handle still refers to the
    // path's current inode; otherwise another process recreated it and the
    // unlink would drop their lock token.
    if (_exclusive && fstat(static_cast<int>(_handle), &handle_info) == 0
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
    const std::filesystem::path& path, const FileLockRequest& request)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return FileLockError { };
    }
#ifdef _WIN32
    // Named mutexes are exclusive only; shared requests degrade to exclusive.
    const std::wstring name = mutex_name(path);
    HANDLE handle           = CreateMutexW(nullptr, FALSE, name.c_str());
    if (handle == nullptr) {
        return FileLockError { };
    }
    const DWORD timeout = request.blocking ? INFINITE : static_cast<DWORD>(100);
    const DWORD result  = WaitForSingleObject(handle, timeout);
    if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) {
        CloseHandle(handle);
        return FileLockError { };
    }
    return FileLock { reinterpret_cast<std::intptr_t>(handle), path, true };
#else
    while (true) {
        const int handle = open(path.c_str(), O_CREAT | O_RDWR, 0600);
        if (handle < 0) {
            return FileLockError { };
        }
        if (flock(handle, flock_operation(request)) != 0) {
            close(handle);
            return FileLockError { };
        }

        struct stat handle_info { };
        struct stat path_info { };
        if (fstat(handle, &handle_info) != 0) {
            flock(handle, flock_operation({ }));
            close(handle);
            return FileLockError { };
        }
        if (stat(path.c_str(), &path_info) == 0
            && handle_info.st_dev == path_info.st_dev
            && handle_info.st_ino == path_info.st_ino) {
            return FileLock { handle, path,
                request.mode == FileLockMode::EXCLUSIVE };
        }
        // The file was replaced between open and flock; retry with the fresh
        // inode so the lock guards the path's current identity.
        flock(handle, flock_operation({ }));
        close(handle);
    }
#endif
}

bool file_lock_held(const std::filesystem::path& path)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return false;
    }
    auto probe = acquire_file_lock(
        path, FileLockRequest { FileLockMode::EXCLUSIVE, false });
    return std::holds_alternative<FileLockError>(probe);
}

std::filesystem::path lock_path_for(const std::filesystem::path& file)
{
    return file.string() + ".lock";
}

} // namespace imza
