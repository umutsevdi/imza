#include "conversation/session_store.h"

#include "conversation/session.h"

#include <utility>

namespace imza {

SessionStore::SessionStore()
    : _worker([this] {
        const std::uint64_t generation = _generation.load();
        _publish_if_current(saved_sessions(), generation);
    })
{
}

SessionStore::~SessionStore() = default;

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
    _generation.fetch_add(1);
    const DeleteSessionResult result = delete_saved_session(path);
    _publish(saved_sessions());
    return result;
}

Signal<>::Subscription SessionStore::subscribe(Signal<>::Callback callback)
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
