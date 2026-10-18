#pragma once

#include <filesystem>
#include <memory>
#include <mutex>

#include "common/imza_signal.h"
#include "common/types.h"
#include "platform/config.h"

namespace imza {

// The single owner of the persisted Config document. Every writer goes
// through update(), which reloads the newest on-disk state under the
// interprocess lock, applies the mutation, replaces the file only when it
// changed, and republishes the in-memory snapshot. Readers take snapshot(),
// a copy-on-write view that is never stale and never a reference into
// locked state.
class ConfigStore final : public ApplicationComponent {
public:
    explicit ConfigStore(
        Config initial = { }, std::filesystem::path path = config_path());

    ConfigStore(const ConfigStore&)            = delete;
    ConfigStore& operator=(const ConfigStore&) = delete;

    std::shared_ptr<const Config> snapshot() const;

    // Locked read-modify-write against the file, based on the current
    // snapshot when the file is absent. Publishes on UPDATED.
    ConfigUpdateResult update(const ConfigMutator& mutate);

    // Callbacks run on the publishing thread; keep the Subscription.
    [[nodiscard]] Signal<>::Subscription subscribe_to_change(
        Signal<>::Callback callback);

private:
    mutable std::mutex _mutex;
    std::shared_ptr<Config> _config;
    std::filesystem::path _path;
    Signal<> _changed;
};

} // namespace imza
