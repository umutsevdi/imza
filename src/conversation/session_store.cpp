#include "conversation/session_store.h"

#include "conversation/session.h"

#include <chrono>
#include <utility>

namespace imza {

SessionStore::SessionStore()
    : _worker([this] {
        const std::uint64_t generation = _generation.load();
        _publish_if_current(saved_sessions(), generation);
    })
{
}

SessionStore::~SessionStore() { stop(); }

void SessionStore::stop()
{
    {
        std::lock_guard<std::mutex> lock_guard(_mutex);
        if (!alive()) {
            return;
        }
        mark_dead();
    }
    // The listing worker is a short one-shot scan with no stop token.
    if (_worker.joinable()) {
        _worker.join();
    }
    if (_trim_worker.joinable()) {
        _trim_worker.request_stop();
        _trim_worker.join();
    }
}

bool SessionStore::ready() const { return _ready.load(); }

std::vector<SavedSession> SessionStore::sessions() const
{
    std::lock_guard lock(_mutex);
    return _sessions;
}

Status SessionStore::save(Session& session)
{
    _generation.fetch_add(1);
    const Status status = save_session(session);
    _publish(saved_sessions());
    return status;
}

DeleteSessionResult SessionStore::remove(const std::filesystem::path& path)
{
    // Deleting the chat-active file would deadlock on our own guard, so it
    // is dropped first.
    {
        std::lock_guard<std::mutex> lock_guard(_mutex);
        if (_active_lock && _active_lock->path() == lock_path_for(path)) {
            _active_lock.reset();
            _active_session_path.clear();
        }
    }
    _generation.fetch_add(1);
    const DeleteSessionResult result = delete_saved_session(path);
    _publish(saved_sessions());
    return result;
}

bool SessionStore::is_locked(const std::filesystem::path& path) const
{
    return session_file_locked(path);
}

bool SessionStore::activate(const std::filesystem::path& path)
{
    const std::filesystem::path lock = lock_path_for(path);
    // The previous file's lock is released only after the new file has been
    // locked, so a failed switch keeps the current conversation guarded.
    auto acquired = acquire_file_lock(
        lock, FileLockRequest { FileLockMode::EXCLUSIVE, false });
    if (!std::holds_alternative<FileLock>(acquired)) {
        return false;
    }
    std::lock_guard<std::mutex> lock_guard(_mutex);
    _active_lock         = std::move(std::get<FileLock>(acquired));
    _active_session_path = path;
    return true;
}

void SessionStore::deactivate()
{
    std::lock_guard<std::mutex> lock_guard(_mutex);
    _active_lock.reset();
    _active_session_path.clear();
}

std::filesystem::path SessionStore::_active_path() const
{
    std::lock_guard<std::mutex> lock_guard(_mutex);
    return _active_session_path;
}

bool SessionStore::trim(std::chrono::days age)
{
    if (age <= std::chrono::days { 0 }) {
        return false;
    }
    std::lock_guard<std::mutex> lock_guard(_mutex);
    // alive() is checked under the same mutex stop() flips it under, so no
    // sweep can start once shutdown has begun.
    if (!alive() || _trim_worker.joinable()) {
        return false;
    }
    _generation.fetch_add(1);
    const std::uint64_t generation = _generation.load();
    _trim_worker = std::jthread([this, age, keep = _active_session_path,
                                    generation](std::stop_token stop) {
        if (stop.stop_requested()) {
            return;
        }
        trim_saved_sessions(age, std::chrono::system_clock::now(), keep);
        if (stop.stop_requested()) {
            return;
        }
        _publish_if_current(saved_sessions(), generation);
    });
    return true;
}

Signal<>::Subscription SessionStore::subscribe_to_change(
    Signal<>::Callback callback)
{
    return _changed.subscribe(std::move(callback));
}

void SessionStore::_publish(std::vector<SavedSession> entries)
{
    {
        std::lock_guard lock(_mutex);
        _sessions = std::move(entries);
        _ready.store(true);
    }
    _changed.publish();
}

void SessionStore::_publish_if_current(
    std::vector<SavedSession> entries, std::uint64_t generation)
{
    if (_generation.load() == generation) {
        _publish(std::move(entries));
    }
}

} // namespace imza
