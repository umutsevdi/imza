#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "common/imza_signal.h"
#include "common/types.h"

namespace imza {

enum class NotificationLevel { INFO, WARN, ERROR };

// One transient message shown in the app-wide overlay. `expires_at` is an
// absolute deadline rather than a countdown: the render thread derives the
// remaining time, so no thread mutates the entry after publication.
// `nullopt` means the entry persists until dismissed (the ERROR default).
struct Notification {
    std::size_t id          = 0;
    NotificationLevel level = NotificationLevel::INFO;
    std::string description;
    std::optional<std::chrono::steady_clock::time_point> expires_at;
};

// Newest entries beyond this are dropped, so a runaway publisher cannot
// grow the overlay without bound.
inline constexpr std::size_t MAX_NOTIFICATIONS = 5;

// ttl sentinel: keep the entry until the user dismisses it, whatever the
// level's default. For notices that describe standing state (an available
// release) rather than a moment.
inline constexpr std::chrono::milliseconds NOTIFICATION_PERSIST {
    std::chrono::milliseconds::max()
};

// Thread-safe store behind the overlay: any worker may publish, the UI
// thread snapshots and renders. Long-lived ApplicationComponent; hands out
// copy-on-write snapshots, never a reference into locked state.
class NotificationStore final : public ApplicationComponent {
public:
    // Returns the new entry's id. `ttl` schedules auto-dismissal; omit it
    // to persist until dismiss().
    std::size_t publish(NotificationLevel level, std::string description,
        std::optional<std::chrono::milliseconds> ttl = std::nullopt);
    void dismiss(std::size_t id);
    // Drops every entry whose deadline has passed and returns whether
    // anything was removed; called from the UI animation tick, so the store
    // owns no timer thread.
    bool dismiss_expired();
    // True while any entry still carries a deadline. The UI keeps requesting
    // animation frames on this, so an auto-dismissing notice actually expires
    // even though nothing else is animating.
    bool has_pending_expiry() const;
    std::shared_ptr<const std::vector<Notification>> notifications() const;
    // Callbacks run on the publishing thread; keep the Subscription.
    [[nodiscard]] Signal<>::Subscription subscribe_to_change(
        Signal<>::Callback callback);

    // Requests a terminal alert (OSC 9, or the bell) without adding an
    // overlay entry. The store carries no terminal knowledge: the terminal
    // layer subscribes and encodes the sequence. Headless and non-root
    // states install no subscriber, so nothing is emitted there.
    void alert(std::string message);
    [[nodiscard]] Signal<const std::string&>::Subscription subscribe_to_alert(
        Signal<const std::string&>::Callback callback);

private:
    void _ensure_unique();
    void _publish_locked();

    mutable std::mutex _mutex;
    std::shared_ptr<std::vector<Notification>> _entries
        = std::make_shared<std::vector<Notification>>();
    std::size_t _next_id = 1;
    Signal<> _changed;
    Signal<const std::string&> _alert;
};

} // namespace imza
