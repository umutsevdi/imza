#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/imza_signal.h"
#include "common/types.h"
#include "platform/config.h"

namespace imza {

// OS keychain access for provider secrets. Mode::OS probes the platform store
// on a worker and degrades to plaintext (recording why) when it is unusable,
// so a machine without a keyring still runs. The probe, the per-provider
// resolve, and the legacy migration all run off the startup path: the first
// frame never waits on D-Bus or `security`.
class CredentialSubsystem final : public Subsystem {
public:
    enum class Mode { OS, PLAINTEXT };

    explicit CredentialSubsystem(Mode mode  = Mode::OS,
        std::vector<Connection> connections = { },
        std::filesystem::path config_path   = { });
    ~CredentialSubsystem() override;

    void stop() override;

    CredentialSubsystem(const CredentialSubsystem&)            = delete;
    CredentialSubsystem& operator=(const CredentialSubsystem&) = delete;

    // Empty with NOT_FOUND when absent, CONFIG_ERROR when the call failed.
    std::string get(const std::string& key, Status& status) const;
    // get(), served from the worker's resolve when it already holds the value.
    std::string lookup(const std::string& key, Status& status) const;
    Status set(const std::string& key, const std::string& value);
    Status remove(const std::string& key);
    Status has(const std::string& key) const;

    std::optional<std::string> cached_secret(std::string_view key) const;

    // Both wait for the probe (bounded). Only writers and the startup notice
    // ask: a writer must never guess and spill a secret into the document.
    // Render paths use ready() instead.
    bool is_keychain_available() const;
    std::string unavailable_reason() const;

    bool ready() const { return _ready.load(); }
    // True once the platform probe finished; never blocks. The worker's
    // startup announcement always comes after this, so a caller that sees
    // it can read unavailable_reason() without waiting.
    bool probed() const { return _probed.load(); }
    // The worker publishes its startup announcement before flipping the
    // latch, so a subscriber installed after this returns cannot mistake
    // that announcement for a later change.
    bool wait_ready(std::chrono::milliseconds timeout) const;

    // Bumped on every change to the resolved set. A reader caching a view
    // compares this instead of subscribing, which would re-enter the
    // caller's own lock from a writer's publish.
    std::uint64_t generation() const { return _generation.load(); }

    // Callbacks run on the publishing thread; keep the Subscription.
    [[nodiscard]] Signal<>::Subscription subscribe_to_change(
        Signal<>::Callback callback);

private:
    void _probe();
    void _resolve(const std::vector<Connection>& connections);
    bool _await_probe() const;

    mutable std::mutex _mutex;
    mutable std::condition_variable _ready_cv;
    mutable std::condition_variable _probe_cv;
    std::atomic<bool> _keychain { false };
    std::atomic<bool> _probed { false };
    std::string _reason;
    // Encoded secrets by keychain key, filled by the worker and kept current
    // by set()/remove().
    std::unordered_map<std::string, std::string> _secrets;
    std::atomic<bool> _ready { false };
    std::atomic<std::uint64_t> _generation { 0 };
    std::jthread _worker;
    Signal<> _changed;
};

} // namespace imza
