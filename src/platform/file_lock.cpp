#include "platform/file_lock.h"

#include <system_error>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
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
        std::uint64_t hash = 14695981039346656037ULL;
        for (const wchar_t value : path.wstring()) {
            hash ^= static_cast<std::uint64_t>(value);
            hash *= 1099511628211ULL;
        }
        return L"Local\\imza-file-lock-" + std::to_wstring(hash);
    }

    // Named mutexes are recursive per-thread: a second acquire from the
    // holding thread succeeds where flock would refuse. This registry
    // restores flock's in-process semantics on top of the kernel mutex,
    // which keeps covering the cross-process case. Blocking requests wait
    // on a condition variable until an in-process holder releases; the
    // holder itself never waits here, so no self-deadlock is possible.
    struct LockRegistry {
        struct Entry {
            std::thread::id exclusive_holder;
            std::vector<std::thread::id> shared_holders;
        };
        std::mutex mutex;
        std::condition_variable released;
        std::map<std::wstring, Entry> entries;

        static LockRegistry& instance()
        {
            static LockRegistry registry;
            return registry;
        }

        bool holds_any(const std::wstring& key)
        {
            std::lock_guard lock(mutex);
            const auto found = entries.find(key);
            if (found == entries.end()) {
                return false;
            }
            return found->second.exclusive_holder != std::thread::id()
                || !found->second.shared_holders.empty();
        }

        // Exclusive admits one holder and no shared holders; shared admits
        // any number as long as no exclusive holder is live.
        bool can_acquire(const std::wstring& key, const FileLockMode mode,
            const bool blocking)
        {
            std::unique_lock lock(mutex);
            const std::thread::id self = std::this_thread::get_id();
            const bool exclusive       = mode == FileLockMode::EXCLUSIVE;
            const auto free_for        = [&](const Entry& entry) {
                if (!exclusive) {
                    return entry.exclusive_holder == std::thread::id();
                }
                return entry.exclusive_holder == std::thread::id()
                    && entry.shared_holders.empty();
            };
            // Same-thread holds make a blocking wait a guaranteed deadlock:
            // refuse immediately instead.
            auto found = entries.find(key);
            if (blocking) {
                if (found != entries.end()
                    && found->second.exclusive_holder == self) {
                    return false;
                }
                // Bounded: a registered holder that vanishes without
                // releasing (abandoned state) must not park later
                // acquirers forever; failing the acquire reports the
                // contention instead of hanging the caller.
                const auto free_at = std::chrono::steady_clock::now()
                    + std::chrono::seconds(10);
                if (!released.wait_until(lock, free_at, [&] {
                        const auto now = entries.find(key);
                        return now == entries.end() || free_for(now->second);
                    })) {
                    return false;
                }
            } else {
                if (found != entries.end() && !free_for(found->second)) {
                    return false;
                }
            }
            if (exclusive) {
                entries[key].exclusive_holder = self;
            } else {
                entries[key].shared_holders.push_back(self);
            }
            return true;
        }

        void release(const std::wstring& key, const FileLockMode mode)
        {
            {
                std::lock_guard lock(mutex);
                const auto found = entries.find(key);
                if (found == entries.end()) {
                    return;
                }
                if (mode == FileLockMode::EXCLUSIVE) {
                    if (found->second.exclusive_holder
                        == std::this_thread::get_id()) {
                        found->second.exclusive_holder = std::thread::id();
                    }
                } else {
                    std::erase(found->second.shared_holders,
                        std::this_thread::get_id());
                }
                if (found->second.exclusive_holder == std::thread::id()
                    && found->second.shared_holders.empty()) {
                    entries.erase(found);
                }
            }
            released.notify_all();
        }
    };
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
    LockRegistry::instance().release(mutex_name(_path),
        _exclusive ? FileLockMode::EXCLUSIVE : FileLockMode::SHARED);
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
    const std::wstring name = mutex_name(path);
    auto& registry          = LockRegistry::instance();
    // Undoes a registry entry whose kernel acquire failed or timed out.
    const auto abandon = [&](HANDLE handle) {
        registry.release(name, request.mode);
        if (handle != nullptr) {
            CloseHandle(handle);
        }
        return FileLockError { };
    };
    if (!registry.can_acquire(name, request.mode, request.blocking)) {
        return FileLockError { };
    }
    HANDLE handle = CreateMutexW(nullptr, FALSE, name.c_str());
    if (handle == nullptr) {
        return abandon(nullptr);
    }
    // Even blocking requests wait on the kernel mutex with a deadline: the
    // registry only arbitrates in-process holders, and a registered waiter
    // parked on INFINITE turns any lost wakeup into a permanent hang that
    // stop()'s worker joins inherit. 15s covers the longest legitimate
    // in-process critical section many times over.
    const DWORD timeout = request.blocking ? static_cast<DWORD>(15000)
                                           : static_cast<DWORD>(100);
    const DWORD result  = WaitForSingleObject(handle, timeout);
    if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) {
        return abandon(handle);
    }
    return FileLock { reinterpret_cast<std::intptr_t>(handle), path,
        request.mode == FileLockMode::EXCLUSIVE };
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
            flock(handle, LOCK_UN);
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
        flock(handle, LOCK_UN);
        close(handle);
    }
#endif
}

bool foreign_lock_live(const std::filesystem::path& path)
{
#ifdef _WIN32
    const std::wstring name = mutex_name(path);
    // The registry sees holders the recursive mutex would hide from a
    // fresh acquirer, including this thread's own holds. A waiting probe
    // on a self-owned mutex would succeed recursively and ReleaseMutex
    // would drop the caller's ownership, so the kernel probe below runs
    // only when the registry reports no in-process holder at all.
    if (LockRegistry::instance().holds_any(name)) {
        return true;
    }
    // A named mutex reports only WAIT_TIMEOUT to a second waiter, so probe
    // with a zero-timeout exclusive acquire and let go: WAIT_OBJECT_0 here
    // means nobody else holds it, WAIT_TIMEOUT means a live holder.
    HANDLE handle = CreateMutexW(nullptr, FALSE, name.c_str());
    if (handle == nullptr) {
        return false;
    }
    const DWORD result = WaitForSingleObject(handle, 0);
    ReleaseMutex(handle);
    CloseHandle(handle);
    return result == WAIT_TIMEOUT;
#else
    // A failed non-blocking exclusive flock is proof of a live holder; on
    // success the probe itself took the lock, so release at once. The file
    // is opened without O_CREAT: a missing lock file means no holder.
    const int handle = open(path.c_str(), O_RDWR);
    if (handle < 0) {
        return false;
    }
    if (flock(handle, LOCK_EX | LOCK_NB) == 0) {
        flock(handle, LOCK_UN);
        close(handle);
        return false;
    }
    close(handle);
    return true;
#endif
}

bool file_lock_held(const std::filesystem::path& path)
{
#ifdef _WIN32
    // No lock file exists on this backend; ask the registry and the named
    // mutex directly.
    return foreign_lock_live(path);
#else
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return false;
    }
    auto probe = acquire_file_lock(
        path, FileLockRequest { FileLockMode::EXCLUSIVE, false });
    return std::holds_alternative<FileLockError>(probe);
#endif
}

std::filesystem::path lock_path_for(const std::filesystem::path& file)
{
    return file.string() + ".lock";
}

} // namespace imza
