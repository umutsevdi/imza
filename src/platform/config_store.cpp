#include "platform/config_store.h"

#include "platform/credential_store.h"

#include <utility>

namespace imza {

ConfigStore::ConfigStore(
    Config initial, std::filesystem::path path, CredentialStore* credentials)
    : _config(std::make_shared<Config>(std::move(initial)))
    , _path(std::move(path))
    , _credentials(credentials)
{
}

std::shared_ptr<const Config> ConfigStore::snapshot() const
{
    std::lock_guard lock(_mutex);
    return _config;
}

ConfigUpdateResult ConfigStore::update(const ConfigMutator& mutate)
{
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
