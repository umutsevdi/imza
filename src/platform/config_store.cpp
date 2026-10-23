#include "platform/config_store.h"

#include "platform/credential_subsystem.h"

#include <chrono>
#include <utility>

namespace imza {

ConfigStore::ConfigStore(Config initial, std::filesystem::path path,
    CredentialSubsystem* credentials)
    : _config(std::make_shared<Config>(std::move(initial)))
    , _path(std::move(path))
    , _credentials(credentials)
{
}

void ConfigStore::_overlay(Config& config) const
{
    if (_credentials == nullptr) {
        return;
    }
    // Only the worker's resolved map is consulted: asking whether a keychain
    // exists would block this reader on the probe.
    for (Connection& connection : config.providers) {
        const std::optional<std::string> secret
            = _credentials->cached_secret(credential_key(connection));
        if (secret) {
            connection.decode_secret(*secret);
        }
    }
}

std::shared_ptr<const Config> ConfigStore::snapshot() const
{
    std::lock_guard lock(_mutex);
    if (_credentials == nullptr) {
        return _config;
    }
    // Cached, not rebuilt per call: this runs every frame. The generation is
    // compared rather than subscribed to, because a credential write happens
    // inside update()'s own critical section and a publish there would
    // re-enter this mutex.
    const std::uint64_t generation = _credentials->generation();
    if (_view == nullptr || _view_generation != generation) {
        auto view = std::make_shared<Config>(*_config);
        _overlay(*view);
        _view            = std::move(view);
        _view_generation = generation;
    }
    return _view;
}

ConfigUpdateResult ConfigStore::update(const ConfigMutator& mutate)
{
    if (_credentials != nullptr) {
        // A save must not race the resolve: update_config re-reads the
        // keychain itself, and a half-resolved document would be written
        // without the secrets it meant to keep.
        _credentials->wait_ready(std::chrono::seconds { 10 });
    }
    ConfigUpdateResult result = ConfigUpdateResult::FAILURE;
    {
        std::lock_guard lock(_mutex);
        Config candidate;
        result
            = update_config(_path, *_config, mutate, &candidate, _credentials);
        if (result != ConfigUpdateResult::FAILURE) {
            // Copy-on-write: a reader still holding the previous snapshot
            // keeps reading immutable memory.
            _config = std::make_shared<Config>(std::move(candidate));
            _view.reset();
        }
    }
    if (result == ConfigUpdateResult::UPDATED) {
        _changed.publish();
    }
    return result;
}

Signal<>::Subscription ConfigStore::subscribe_to_change(
    Signal<>::Callback callback)
{
    return _changed.subscribe(std::move(callback));
}

} // namespace imza
