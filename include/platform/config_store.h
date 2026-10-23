#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>

#include "common/imza_signal.h"
#include "common/types.h"
#include "platform/config.h"

namespace imza {

class CredentialSubsystem;

// The single owner of the persisted Config document. Every writer goes
// through update(), which reloads the newest on-disk state under the
// interprocess lock, applies the mutation, replaces the file only when it
// changed, and republishes the in-memory snapshot. Readers take snapshot(),
// a copy-on-write view that is never stale and never a reference into
// locked state.
//
// The document on disk carries no secrets in keychain mode. snapshot()
// overlays the ones the credential worker resolved, so `connection.api_key`
// stays the single read path and no caller has to know where a key lives.
class ConfigStore final : public Subsystem {
public:
    // `credentials` is borrowed, not owned: ApplicationState owns the store
    // and outlives this one. When it is null, or has no keychain, the
    // document's own plaintext secrets are served as they are.
    explicit ConfigStore(Config initial  = { },
        std::filesystem::path path       = config_path(),
        CredentialSubsystem* credentials = nullptr);

    ConfigStore(const ConfigStore&)            = delete;
    ConfigStore& operator=(const ConfigStore&) = delete;

    std::shared_ptr<const Config> snapshot() const;

    // Locked read-modify-write against the file, based on the current
    // snapshot when the file is absent. Publishes on UPDATED. Waits for the
    // credential worker first, so a save cannot race the resolve and write a
    // document without the secrets it meant to keep.
    ConfigUpdateResult update(const ConfigMutator& mutate);

    // Callbacks run on the publishing thread; keep the Subscription.
    [[nodiscard]] Signal<>::Subscription subscribe_to_change(
        Signal<>::Callback callback);

private:
    // `config` with every connection's secret filled from the worker's
    // resolve. A connection the keychain has no entry for keeps whatever the
    // document carried.
    void _overlay(Config& config) const;

    mutable std::mutex _mutex;
    std::shared_ptr<Config> _config;
    // The overlaid view snapshot() hands out; rebuilt when the document or
    // the credential worker changes.
    mutable std::shared_ptr<const Config> _view;
    std::filesystem::path _path;
    CredentialSubsystem* _credentials = nullptr;
    // The credential generation _view was built from.
    mutable std::uint64_t _view_generation = 0;
    Signal<> _changed;
};

} // namespace imza
