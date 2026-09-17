#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "common/imza_signal.h"
#include "common/types.h"
#include "conversation/persistence.h"
#include "platform/file_lock.h"

namespace imza {

class Session;

class SessionStore final : public ApplicationComponent {
public:
    SessionStore();
    ~SessionStore();

    bool ready() const;
    std::vector<SavedSession> sessions() const;
    Status save(Session& session);
    DeleteSessionResult remove(const std::filesystem::path& path);
    // True while another process holds the session's lock file.
    bool is_locked(const std::filesystem::path& path) const;
    // Chat-active guard for the running session's file. Fails when another
    // process holds it; releases any previously held session file first.
    bool activate(const std::filesystem::path& path);
    void deactivate();
    [[nodiscard]] Signal<>::Subscription subscribe(Signal<>::Callback callback);

private:
    void _publish(std::vector<SavedSession> entries);
    void _publish_if_current(
        std::vector<SavedSession> entries, std::uint64_t generation);

    mutable std::mutex _mutex;
    std::vector<SavedSession> _sessions;
    std::optional<FileLock> _active_lock;
    std::atomic<bool> _ready { false };
    std::atomic<std::uint64_t> _generation { 0 };
    Signal<> _changed;
    std::jthread _worker;
};

} // namespace imza
