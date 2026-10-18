#include "runtime/notification_center.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace imza {

namespace {
    // INFO and WARN are transient; ERROR has no deadline and waits for the
    // user. Callers may override with an explicit ttl.
    std::optional<std::chrono::milliseconds> default_ttl(
        NotificationLevel level)
    {
        switch (level) {
        case NotificationLevel::INFO: return std::chrono::seconds { 4 };
        case NotificationLevel::WARN: return std::chrono::seconds { 8 };
        case NotificationLevel::ERROR: return std::nullopt;
        }
        return std::nullopt;
    }
} // namespace

void NotificationStore::_ensure_unique()
{
    if (!_entries.unique()) {
        _entries = std::make_shared<std::vector<Notification>>(*_entries);
    }
}

void NotificationStore::_publish_locked() { _changed.publish(); }

std::size_t NotificationStore::publish(NotificationLevel level,
    std::string description, std::optional<std::chrono::milliseconds> ttl)
{
    if (description.empty()) {
        return 0;
    }
    std::size_t id = 0;
    {
        std::lock_guard lock(_mutex);
        id = _next_id++;
        _ensure_unique();
        Notification entry;
        entry.id          = id;
        entry.level       = level;
        entry.description = std::move(description);
        const auto window = ttl ? ttl : default_ttl(level);
        if (window && *window != NOTIFICATION_PERSIST) {
            entry.expires_at = std::chrono::steady_clock::now() + *window;
        }
        _entries->push_back(std::move(entry));
        while (_entries->size() > MAX_NOTIFICATIONS) {
            _entries->erase(_entries->begin());
        }
    }
    _publish_locked();
    return id;
}

void NotificationStore::dismiss(std::size_t id)
{
    bool removed = false;
    {
        std::lock_guard lock(_mutex);
        const bool present = std::any_of(_entries->begin(), _entries->end(),
            [id](const Notification& entry) { return entry.id == id; });
        if (present) {
            _ensure_unique();
            std::erase_if(*_entries,
                [id](const Notification& entry) { return entry.id == id; });
            removed = true;
        }
    }
    if (removed) {
        _publish_locked();
    }
}

bool NotificationStore::dismiss_expired()
{
    bool removed = false;
    {
        std::lock_guard lock(_mutex);
        const auto now = std::chrono::steady_clock::now();
        const bool any = std::any_of(_entries->begin(), _entries->end(),
            [now](const Notification& entry) {
                return entry.expires_at && *entry.expires_at <= now;
            });
        if (any) {
            _ensure_unique();
            std::erase_if(*_entries, [now](const Notification& entry) {
                return entry.expires_at && *entry.expires_at <= now;
            });
            removed = true;
        }
    }
    if (removed) {
        _publish_locked();
    }
    return removed;
}

bool NotificationStore::has_pending_expiry() const
{
    std::lock_guard lock(_mutex);
    return std::any_of(_entries->begin(), _entries->end(),
        [](const Notification& entry) { return entry.expires_at.has_value(); });
}

std::shared_ptr<const std::vector<Notification>>
NotificationStore::notifications() const
{
    std::lock_guard lock(_mutex);
    return _entries;
}

Signal<>::Subscription NotificationStore::subscribe_to_change(
    Signal<>::Callback callback)
{
    return _changed.subscribe(std::move(callback));
}

void NotificationStore::alert(std::string message)
{
    if (!message.empty()) {
        _alert.publish(message);
    }
}

Signal<const std::string&>::Subscription NotificationStore::subscribe_to_alert(
    Signal<const std::string&>::Callback callback)
{
    return _alert.subscribe(std::move(callback));
}

} // namespace imza
