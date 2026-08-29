#include "provider_store.h"

#include "pricing.h"

#include <algorithm>
#include <utility>

namespace ursa {

ProviderStore::ProviderStore(Config config, ModelsFn models_fn)
    : config_(std::move(config))
    , models_fn_(std::move(models_fn))
{
    load_catalog(presets_path(), catalog_);
    set_pricing_catalog(catalog_);
    if (!models_fn_) {
        models_fn_ = [](const Route& route, std::vector<ModelInfo>& models) {
            return fetch_models(route, models);
        };
    }
}

ProviderStore::~ProviderStore()
{
    alive_.store(false);
    catalog_worker_.reset();
    workers_.clear();
}

Signal<>::Subscription ProviderStore::subscribe(ProviderChangedFn callback)
{
    return changed_.subscribe(std::move(callback));
}

Config ProviderStore::config() const
{
    std::lock_guard lock(mutex_);
    return config_;
}

StatusConfigView ProviderStore::status() const
{
    std::lock_guard lock(mutex_);
    return { config_.last_used ? config_.last_used->model : "",
        config_.reasoning_effort.value_or("off") };
}

std::vector<ConnectionView> ProviderStore::connections() const
{
    std::lock_guard lock(mutex_);
    std::vector<ConnectionView> views;
    views.reserve(config_.providers.size());
    for (const Connection& connection : config_.providers) {
        ConnectionView view;
        view.id          = connection.id;
        view.provider_id = connection.provider_id;
        view.api_key     = connection.api_key;
        view.active
            = config_.last_used && config_.last_used->provider == connection.id;
        if (connection.provider_id == kLocalProviderId) {
            view.name = "Local";
        } else if (connection.provider_id == kCustomProviderId) {
            view.name = "Custom";
        } else if (const auto it
            = catalog_.providers.find(connection.provider_id);
            it != catalog_.providers.end()) {
            view.name = it->second.name;
        }
        if (view.name.empty()) {
            view.name = connection.provider_id;
        }
        const auto it = model_catalog_.find(connection.id);
        if (it != model_catalog_.end()) {
            if (const auto* ready
                = std::get_if<CatalogEntry::Ready>(&it->second.state)) {
                view.state       = ConnectionView::State::READY;
                view.model_count = ready->models.size();
            } else if (const auto* failed
                = std::get_if<CatalogEntry::Failed>(&it->second.state)) {
                view.state = ConnectionView::State::FAILED;
                view.error = failed->status;
            }
        }
        views.push_back(std::move(view));
    }
    return views;
}

ModelList ProviderStore::models_for(std::string_view connection_id) const
{
    std::lock_guard lock(mutex_);
    ModelList list;
    const Connection* connection = _find_locked(connection_id);
    if (connection == nullptr) {
        list.state = ModelList::State::FAILED;
        list.error = Status::CONFIG_ERROR;
        return list;
    }
    const auto it = model_catalog_.find(std::string(connection_id));
    if (it == model_catalog_.end()) {
        return list;
    }
    const auto* ready = std::get_if<CatalogEntry::Ready>(&it->second.state);
    if (ready == nullptr) {
        if (const auto* failed
            = std::get_if<CatalogEntry::Failed>(&it->second.state)) {
            list.state = ModelList::State::FAILED;
            list.error = failed->status;
        }
        return list;
    }
    list.state          = ModelList::State::READY;
    list.models         = ready->models;
    const auto provider = catalog_.providers.find(connection->provider_id);
    if (provider == catalog_.providers.end()) {
        return list;
    }
    for (ModelInfo& info : list.models) {
        if (info.context_length.has_value()) {
            continue;
        }
        const auto model = provider->second.models.find(info.id);
        if (model != provider->second.models.end() && model->second.context) {
            info.context_length = model->second.context;
        }
    }
    return list;
}

std::vector<std::pair<std::string, std::string>>
ProviderStore::provider_options() const
{
    std::lock_guard lock(mutex_);
    std::vector<std::pair<std::string, std::string>> options;
    options.reserve(catalog_.providers.size() + 2);
    for (const auto& [id, provider] : catalog_.providers) {
        options.emplace_back(id, provider.name.empty() ? id : provider.name);
    }
    std::sort(options.begin(), options.end());
    options.emplace_back(std::string(kLocalProviderId), "Local");
    options.emplace_back(std::string(kCustomProviderId), "Custom");
    return options;
}

std::optional<ProviderSelection> ProviderStore::active_selection() const
{
    std::lock_guard lock(mutex_);
    if (!config_.last_used || config_.last_used->model.empty()) {
        return std::nullopt;
    }
    const Connection* connection = _find_locked(config_.last_used->provider);
    if (connection == nullptr) {
        return std::nullopt;
    }
    ApiStandard dialect = ApiStandard::OPENAI;
    if (const auto it = connection->dialects.find(config_.last_used->model);
        it != connection->dialects.end()) {
        dialect = it->second;
    }
    return ProviderSelection { config_.last_used->model,
        config_.reasoning_effort.value_or("off"), connection->id,
        _route_locked(*connection, dialect) };
}

Route ProviderStore::route_for(
    std::string_view connection_id, ApiStandard dialect) const
{
    std::lock_guard lock(mutex_);
    const Connection* connection = _find_locked(connection_id);
    return connection == nullptr ? Route { }
                                 : _route_locked(*connection, dialect);
}

bool ProviderStore::model_reasons(std::string_view model) const
{
    if (model.empty()) {
        return false;
    }
    std::lock_guard lock(mutex_);
    for (const auto& [provider_id, provider] : catalog_.providers) {
        const auto it = provider.models.find(std::string(model));
        if (it != provider.models.end() && it->second.reasoning == true) {
            return true;
        }
    }
    return false;
}

void ProviderStore::start_model_fetches()
{
    {
        std::lock_guard lock(mutex_);
        for (const Connection& connection : config_.providers) {
            _start_fetch_locked(connection.id);
        }
    }
    _notify_changed();
}

void ProviderStore::refetch_models(std::string_view connection_id)
{
    {
        std::lock_guard lock(mutex_);
        _start_fetch_locked(std::string(connection_id));
    }
    _notify_changed();
}

void ProviderStore::connect(ConnectResult result, ConnectCompleteFn complete)
{
    Route route;
    {
        std::lock_guard lock(mutex_);
        const bool known = result.provider_id == kLocalProviderId
            || result.provider_id == kCustomProviderId
            || catalog_.providers.contains(result.provider_id);
        if (known) {
            Connection probe;
            probe.provider_id = result.provider_id;
            probe.endpoint    = result.endpoint;
            probe.api_key     = result.api_key;
            route             = _route_locked(probe, ApiStandard::OPENAI);
        }
    }
    if (route.endpoint.empty()) {
        complete(ConnectOutcome { Status::INVALID_URL });
        return;
    }

    std::lock_guard lock(mutex_);
    workers_.emplace_back([this, result = std::move(result), route,
                              complete = std::move(complete)] {
        std::vector<ModelInfo> models;
        const Status fetched = models_fn_(route, models);
        if (!alive_.load()) {
            return;
        }
        ConnectOutcome outcome;
        outcome.status      = fetched;
        outcome.model_count = models.size();
        if (fetched == Status::OK && result.persist) {
            std::lock_guard lock(mutex_);
            outcome.status = _commit_connection_locked(
                result, models, outcome.first_connection);
            outcome.persisted = outcome.status == Status::OK;
        }
        if (outcome.persisted) {
            _notify_changed();
        }
        complete(outcome);
    });
}

bool ProviderStore::remove_connection(std::string_view connection_id)
{
    {
        std::lock_guard lock(mutex_);
        if (config_.providers.size() <= 1) {
            return false;
        }
        Config candidate    = config_;
        auto& providers     = candidate.providers;
        const auto old_size = providers.size();
        providers.erase(std::remove_if(providers.begin(), providers.end(),
                            [connection_id](const Connection& connection) {
                                return connection.id == connection_id;
                            }),
            providers.end());
        if (providers.size() == old_size) {
            return false;
        }
        if (candidate.last_used
            && candidate.last_used->provider == connection_id) {
            candidate.last_used = LastUsed { providers.front().id, "" };
        }
        if (save_config(config_path(), candidate) != Status::OK) {
            return false;
        }
        config_ = std::move(candidate);
        ++generations_[std::string(connection_id)];
        model_catalog_.erase(std::string(connection_id));
    }
    _notify_changed();
    return true;
}

bool ProviderStore::select_model(const ModelChoice& choice)
{
    {
        std::lock_guard lock(mutex_);
        if (_find_locked(choice.connection_id) == nullptr) {
            return false;
        }
        Config candidate = config_;
        candidate.last_used
            = LastUsed { choice.connection_id, choice.model_id };
        if (save_config(config_path(), candidate) != Status::OK) {
            return false;
        }
        config_ = std::move(candidate);
    }
    _notify_changed();
    return true;
}

bool ProviderStore::set_reasoning_effort(std::string effort)
{
    {
        std::lock_guard lock(mutex_);
        Config candidate           = config_;
        candidate.reasoning_effort = std::move(effort);
        if (save_config(config_path(), candidate) != Status::OK) {
            return false;
        }
        config_ = std::move(candidate);
    }
    _notify_changed();
    return true;
}

void ProviderStore::remember_dialect(
    std::string_view connection_id, std::string_view model, ApiStandard dialect)
{
    {
        std::lock_guard lock(mutex_);
        Config candidate = config_;
        auto it          = std::find_if(candidate.providers.begin(),
            candidate.providers.end(), [connection_id](const Connection& item) {
                return item.id == connection_id;
            });
        if (it == candidate.providers.end()) {
            return;
        }
        it->dialects[std::string(model)] = dialect;
        if (save_config(config_path(), candidate) != Status::OK) {
            return;
        }
        config_ = std::move(candidate);
    }
    _notify_changed();
}

void ProviderStore::ensure_catalog_fresh()
{
    {
        std::lock_guard lock(mutex_);
        if (catalog_syncing_ || !catalog_stale(catalog_)) {
            return;
        }
        catalog_syncing_ = true;
    }
    catalog_worker_.emplace([this] {
        Catalog catalog;
        const Status status = fetch_catalog(catalog);
        if (!alive_.load()) {
            return;
        }
        {
            std::lock_guard lock(mutex_);
            catalog_syncing_ = false;
            if (status != Status::OK) {
                return;
            }
            catalog_ = catalog;
            save_catalog(presets_path(), catalog_);
            set_pricing_catalog(catalog_);
        }
        _notify_changed();
    });
}

Connection* ProviderStore::_find_locked(std::string_view id)
{
    const auto it
        = std::find_if(config_.providers.begin(), config_.providers.end(),
            [id](const Connection& connection) { return connection.id == id; });
    return it == config_.providers.end() ? nullptr : &*it;
}

const Connection* ProviderStore::_find_locked(std::string_view id) const
{
    const auto it
        = std::find_if(config_.providers.begin(), config_.providers.end(),
            [id](const Connection& connection) { return connection.id == id; });
    return it == config_.providers.end() ? nullptr : &*it;
}

std::string ProviderStore::_unique_id_locked(std::string base) const
{
    if (_find_locked(base) == nullptr) {
        return base;
    }
    for (int suffix = 2;; ++suffix) {
        std::string candidate = base + "-" + std::to_string(suffix);
        if (_find_locked(candidate) == nullptr) {
            return candidate;
        }
    }
}

Route ProviderStore::_route_locked(
    const Connection& connection, ApiStandard dialect) const
{
    return resolve_route(connection, catalog_, dialect);
}

void ProviderStore::_start_fetch_locked(const std::string& connection_id)
{
    const Connection* connection = _find_locked(connection_id);
    if (connection == nullptr) {
        return;
    }
    const Route route    = _route_locked(*connection, ApiStandard::OPENAI);
    const int generation = ++generations_[connection_id];
    if (route.api.empty()) {
        model_catalog_[connection_id]
            = CatalogEntry { CatalogEntry::Failed { Status::INVALID_URL } };
        return;
    }
    model_catalog_[connection_id] = CatalogEntry { CatalogEntry::Fetching { } };
    workers_.emplace_back([this, connection_id, generation, route] {
        std::vector<ModelInfo> models;
        const Status status = models_fn_(route, models);
        if (!alive_.load()) {
            return;
        }
        {
            std::lock_guard lock(mutex_);
            if (generations_[connection_id] != generation) {
                return;
            }
            if (status == Status::OK) {
                model_catalog_[connection_id] = CatalogEntry {
                    CatalogEntry::Ready { std::move(models) }
                };
            } else {
                model_catalog_[connection_id]
                    = CatalogEntry { CatalogEntry::Failed { status } };
            }
        }
        _notify_changed();
    });
}

Status ProviderStore::_commit_connection_locked(const ConnectResult& result,
    const std::vector<ModelInfo>& models, bool& first)
{
    Config candidate = config_;
    first            = !candidate.last_used.has_value();

    Connection probe;
    probe.provider_id = result.provider_id;
    probe.endpoint    = result.endpoint;
    probe.api_key     = result.api_key;
    const Route route = resolve_route(probe, catalog_, ApiStandard::OPENAI);

    Connection stored;
    stored.provider_id = result.provider_id;
    stored.api_key     = result.api_key;
    if (result.provider_id == kLocalProviderId
        || result.provider_id == kCustomProviderId) {
        stored.endpoint = result.endpoint;
    }

    Connection* existing = nullptr;
    for (Connection& connection : candidate.providers) {
        const Route other
            = resolve_route(connection, catalog_, ApiStandard::OPENAI);
        if (!route.endpoint.empty() && other.endpoint == route.endpoint) {
            existing = &connection;
            break;
        }
    }

    std::string id;
    if (existing != nullptr) {
        existing->provider_id = stored.provider_id;
        existing->api_key     = stored.api_key;
        existing->endpoint    = stored.endpoint;
        existing->dialects.clear();
        id = existing->id;
    } else {
        stored.id = _unique_id_locked(result.provider_id);
        id        = stored.id;
        candidate.providers.push_back(std::move(stored));
    }
    if (save_config(config_path(), candidate) != Status::OK) {
        return Status::CONFIG_ERROR;
    }
    config_ = std::move(candidate);
    ++generations_[id];
    model_catalog_[id] = CatalogEntry { CatalogEntry::Ready { models } };
    return Status::OK;
}

void ProviderStore::_notify_changed()
{
    changed_.publish();
}

} // namespace ursa
