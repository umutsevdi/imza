#include "providers/store.h"

#include "common/util.h"
#include "providers/pricing.h"
#include "providers/subscriptions.h"

#include <algorithm>
#include <ctime>
#include <utility>

namespace imza {

namespace {

    const Connection* find_connection(
        const std::vector<Connection>& providers, std::string_view key)
    {
        for (const Connection& connection : providers) {
            if (connection_key(connection) == key) {
                return &connection;
            }
        }
        return nullptr;
    }

    ApiStandard default_dialect(
        const Connection& connection, const Catalog& catalog)
    {
        if (connection.id == OPENAI_SUBSCRIPTION_ID) {
            return ApiStandard::OPENAI_RESPONSES;
        }
        if (!connection.endpoint.empty()) {
            return ApiStandard::OPENAI;
        }
        const auto provider = catalog.providers.find(connection.id);
        return provider == catalog.providers.end()
            ? ApiStandard::OPENAI
            : dialect_from_npm(provider->second.npm);
    }

    bool subscription_connection(std::string_view id)
    {
        return id == OPENAI_SUBSCRIPTION_ID;
    }

    std::vector<ModelInfo> catalog_models(
        const Catalog& catalog, std::string_view id)
    {
        std::vector<ModelInfo> models;
        const auto provider = catalog.providers.find(std::string(id));
        if (provider == catalog.providers.end()) {
            return models;
        }
        models.reserve(provider->second.models.size());
        for (const auto& [model_id, cached] : provider->second.models) {
            ModelInfo model;
            model.id             = model_id;
            model.name           = cached.name;
            model.context_length = cached.context;
            model.capabilities   = cached.capabilities;
            models.push_back(std::move(model));
        }
        return models;
    }

} // namespace

std::string subagent_variant_or_default(
    const SubagentModelConfig* configured, SubagentRole role)
{
    return configured != nullptr && !configured->variant.empty()
        ? configured->variant
        : std::string(subagent_default_variant(role));
}

ProviderStore::ProviderStore(Config config, ModelsFn models_fn)
    : _config(std::move(config))
    , _models_fn(std::move(models_fn))
{
    load_catalog(presets_path(), _catalog);
    inject_subscription_providers(_catalog);
    _pricing = pricing_table_from(_catalog);
    if (!_models_fn) {
        _models_fn = [](const Route& route, std::vector<ModelInfo>& models) {
            return fetch_models(route, models);
        };
    }
}

ProviderStore::~ProviderStore()
{
    alive_.store(false);
    _catalog_worker.reset();
    _workers.clear();
}

Signal<>::Subscription ProviderStore::subscribe(ProviderChangedFn callback)
{
    return changed_.subscribe(std::move(callback));
}

Config ProviderStore::config() const
{
    std::lock_guard lock(_mutex);
    return _config;
}

StatusConfigView ProviderStore::status() const
{
    std::lock_guard lock(_mutex);
    return { _config.last_used ? _config.last_used->model : "",
        _config.reasoning_effort.value_or("off") };
}

std::vector<ConnectionView> ProviderStore::connections() const
{
    std::lock_guard lock(_mutex);
    std::vector<ConnectionView> views;
    views.reserve(_config.providers.size());
    for (const Connection& connection : _config.providers) {
        ConnectionView view;
        view.id       = connection_key(connection);
        view.provider = connection.id;
        view.api_key  = connection.api_key;
        if (subscription_connection(connection.id)) {
            view.name = _catalog.providers.at(connection.id).name;
        } else if (!connection.endpoint.empty()) {
            view.name = "Custom";
        } else if (const auto it = _catalog.providers.find(connection.id);
            it != _catalog.providers.end()) {
            view.name = it->second.name;
        }
        if (view.name.empty()) {
            view.name = connection.id;
        }
        if (!connection.label.empty()) {
            view.name += " · " + connection.label;
        }
        const auto it = _model_catalog.find(connection_key(connection));
        if (it != _model_catalog.end()) {
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
    std::lock_guard lock(_mutex);
    ModelList list;
    const Connection* connection = _find_locked(connection_id);
    if (connection == nullptr) {
        list.state = ModelList::State::FAILED;
        list.error = Status::CONFIG_ERROR;
        return list;
    }
    const auto it = _model_catalog.find(std::string(connection_id));
    if (it == _model_catalog.end()) {
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
    const auto provider = _catalog.providers.find(connection->id);
    if (provider == _catalog.providers.end()) {
        return list;
    }
    for (ModelInfo& info : list.models) {
        const auto model = provider->second.models.find(info.id);
        if (model == provider->second.models.end()) {
            continue;
        }
        if (!info.context_length && model->second.context) {
            info.context_length = model->second.context;
        }
        if (!info.capabilities && model->second.capabilities) {
            info.capabilities = model->second.capabilities;
        }
    }
    return list;
}

std::vector<std::pair<std::string, std::string>>
ProviderStore::provider_options() const
{
    std::lock_guard lock(_mutex);
    std::vector<std::pair<std::string, std::string>> options;
    options.reserve(_catalog.providers.size() + 1);
    options.emplace_back(std::string(OPENAI_SUBSCRIPTION_ID),
        std::string(OPENAI_SUBSCRIPTION_NAME));
    for (const auto& [id, provider] : _catalog.providers) {
        if (id == OPENAI_SUBSCRIPTION_ID) {
            continue;
        }
        options.emplace_back(id, provider.name.empty() ? id : provider.name);
    }
    std::sort(options.begin() + 1, options.end());
    options.emplace_back(std::string(CUSTOM_PROVIDER_ID), "Custom");
    return options;
}

std::optional<ProviderSelection> ProviderStore::active_selection() const
{
    std::lock_guard lock(_mutex);
    if (!_config.last_used || _config.last_used->model.empty()) {
        return std::nullopt;
    }
    const Connection* connection = _find_locked(_config.last_used->provider);
    if (connection == nullptr) {
        return std::nullopt;
    }
    ApiStandard dialect = default_dialect(*connection, _catalog);
    if (const auto it = connection->dialects.find(_config.last_used->model);
        it != connection->dialects.end()) {
        dialect = it->second;
    }
    return ProviderSelection { _config.last_used->model,
        _config.reasoning_effort.value_or("off"), connection_key(*connection),
        _route_locked(*connection, dialect) };
}

std::optional<ProviderSelection> ProviderStore::subagent_selection(
    SubagentRole role) const
{
    std::lock_guard lock(_mutex);
    const auto configured  = _config.subagents.find(role);
    const bool use_default = configured == _config.subagents.end()
        || configured->second.provider.empty();
    if (use_default
        && (!_config.last_used || _config.last_used->model.empty())) {
        return std::nullopt;
    }
    const std::string& provider = use_default ? _config.last_used->provider
                                              : configured->second.provider;
    const std::string& model
        = use_default ? _config.last_used->model : configured->second.model;
    const Connection* connection = _find_locked(provider);
    if (connection == nullptr) {
        return std::nullopt;
    }
    ApiStandard dialect = default_dialect(*connection, _catalog);
    if (const auto found = connection->dialects.find(model);
        found != connection->dialects.end()) {
        dialect = found->second;
    }
    const std::string variant = subagent_variant_or_default(
        configured != _config.subagents.end() ? &configured->second : nullptr,
        role);
    return ProviderSelection { model, variant, connection_key(*connection),
        _route_locked(*connection, dialect) };
}

Route ProviderStore::route_for(
    std::string_view connection_id, ApiStandard dialect) const
{
    std::lock_guard lock(_mutex);
    const Connection* connection = _find_locked(connection_id);
    return connection == nullptr ? Route { }
                                 : _route_locked(*connection, dialect);
}

Route ProviderStore::authenticated_route_for(std::string_view connection_id,
    ApiStandard dialect, std::string_view opencode_session)
{
    const std::string id(connection_id);
    Connection snapshot;
    {
        std::unique_lock lock(_mutex);
        _refresh_changed.wait(lock, [&] { return !_refreshing.contains(id); });
        const Connection* connection = _find_locked(id);
        if (connection == nullptr) {
            return { };
        }
        const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        if (!subscription_connection(connection->id)
            || connection->expires_at == 0
            || now <= connection->expires_at - 300) {
            return _route_locked(*connection, dialect, opencode_session);
        }
        if (connection->refresh_token.empty()) {
            Route route = _route_locked(*connection, dialect, opencode_session);
            route.error = Status::API_ERROR;
            route.error_message = "Subscription expired. Sign in again.";
            return route;
        }
        snapshot = *connection;
        _refreshing.insert(id);
    }

    const SubscriptionResult refreshed = refresh_subscription(
        snapshot.id, snapshot.refresh_token, snapshot.account_id);
    bool persisted = false;
    if (refreshed.status == Status::OK) {
        persisted = _update_config([&](Config& candidate) {
            Connection* connection = const_cast<Connection*>(
                find_connection(candidate.providers, id));
            if (connection == nullptr) {
                return false;
            }
            connection->api_key       = refreshed.credentials.access_token;
            connection->refresh_token = refreshed.credentials.refresh_token;
            connection->expires_at    = refreshed.credentials.expires_at;
            if (!refreshed.credentials.account_id.empty()) {
                connection->account_id = refreshed.credentials.account_id;
            }
            connection->label.clear();
            return true;
        });
    }

    Route route;
    {
        std::lock_guard lock(_mutex);
        _refreshing.erase(id);
        const Connection* connection = _find_locked(id);
        if (connection != nullptr) {
            route = _route_locked(*connection, dialect, opencode_session);
        }
    }
    _refresh_changed.notify_all();
    if (refreshed.status != Status::OK || !persisted) {
        route.error = refreshed.status == Status::OK ? Status::CONFIG_ERROR
                                                     : refreshed.status;
        route.error_message = refreshed.status == Status::OK
            ? error_text(Status::CONFIG_ERROR)
            : refreshed.error;
    }
    return route;
}

bool ProviderStore::model_reasons(std::string_view model) const
{
    if (model.empty()) {
        return false;
    }
    std::lock_guard lock(_mutex);
    for (const auto& [provider_id, provider] : _catalog.providers) {
        const auto it = provider.models.find(std::string(model));
        if (it != provider.models.end() && it->second.reasoning == true) {
            return true;
        }
    }
    return false;
}

ModelPricing ProviderStore::pricing_for(std::string_view model) const
{
    const std::string key = to_lower(model);
    if (key.empty()) {
        return { };
    }
    std::lock_guard lock(_mutex);
    const auto exact = _pricing.find(key);
    if (exact != _pricing.end()) {
        return exact->second;
    }
    for (const auto& [name, row] : _pricing) {
        if (key.find(name) != std::string::npos) {
            return row;
        }
    }
    return { };
}

void ProviderStore::start_model_fetches()
{
    {
        std::lock_guard lock(_mutex);
        for (const Connection& connection : _config.providers) {
            _start_fetch_locked(connection_key(connection));
        }
    }
    _notify_changed();
}

void ProviderStore::refetch_models(std::string_view connection_id)
{
    {
        std::lock_guard lock(_mutex);
        _start_fetch_locked(std::string(connection_id));
    }
    _notify_changed();
}

void ProviderStore::connect(ConnectResult result, ConnectCompleteFn complete)
{
    Route route;
    std::vector<ModelInfo> subscription_models;
    {
        std::lock_guard lock(_mutex);
        const bool known = result.id == CUSTOM_PROVIDER_ID
            || _catalog.providers.contains(result.id);
        if (known) {
            Connection probe;
            probe.id       = result.id;
            probe.endpoint = result.endpoint;
            probe.api_key  = result.api_key;
            route = _route_locked(probe, default_dialect(probe, _catalog));
            if (subscription_connection(result.id)) {
                subscription_models = catalog_models(_catalog, result.id);
            }
        }
    }
    if (route.endpoint.empty()) {
        complete(ConnectOutcome { Status::INVALID_URL });
        return;
    }

    std::lock_guard lock(_mutex);
    _workers.emplace_back(
        [this, result = std::move(result), route,
            subscription_models = std::move(subscription_models),
            complete            = std::move(complete)] {
            std::vector<ModelInfo> models = subscription_models;
            const Status fetched          = subscription_connection(result.id)
                ? Status::OK
                : _models_fn(route, models);
            if (!alive_.load()) {
                return;
            }
            ConnectOutcome outcome;
            outcome.status      = fetched;
            outcome.model_count = models.size();
            if (fetched == Status::OK && result.persist) {
                std::lock_guard lock(_mutex);
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

bool ProviderStore::remove_connection(
    std::size_t index, std::string_view expected_id)
{
    return _update_config(
        [&](Config& candidate) {
            if (candidate.providers.size() <= 1
                || index >= candidate.providers.size()
                || connection_key(candidate.providers[index]) != expected_id) {
                return false;
            }
            auto& providers              = candidate.providers;
            const std::string removed_id = connection_key(providers[index]);
            providers.erase(
                providers.begin() + static_cast<std::ptrdiff_t>(index));
            if (candidate.last_used
                && candidate.last_used->provider == removed_id) {
                candidate.last_used
                    = LastUsed { connection_key(providers.front()), "" };
            }
            std::erase_if(candidate.subagents, [&](const auto& entry) {
                return entry.second.provider == removed_id;
            });
            return true;
        },
        [&] {
            if (_find_locked(expected_id) == nullptr) {
                ++_generations[std::string(expected_id)];
                _model_catalog.erase(std::string(expected_id));
            }
        });
}

bool ProviderStore::select_model(const ModelChoice& choice)
{
    return _update_config([&](Config& candidate) {
        if (find_connection(candidate.providers, choice.connection_id)
            == nullptr) {
            return false;
        }
        candidate.last_used
            = LastUsed { choice.connection_id, choice.model_id };
        return true;
    });
}

bool ProviderStore::set_reasoning_effort(std::string effort)
{
    return _update_config([&](Config& candidate) {
        candidate.reasoning_effort = std::move(effort);
        return true;
    });
}

bool ProviderStore::set_subagent_model(
    SubagentRole role, SubagentModelConfig selection)
{
    return _update_config([&](Config& candidate) {
        if (selection.provider.empty() != selection.model.empty()
            || (!selection.provider.empty()
                && find_connection(candidate.providers, selection.provider)
                    == nullptr)) {
            return false;
        }
        if (selection.variant.empty()) {
            selection.variant = subagent_default_variant(role);
        }
        candidate.subagents[role] = std::move(selection);
        return true;
    });
}

bool ProviderStore::set_skill_policies(const SkillPolicyChanges& changes)
{
    return _update_config([&](Config& candidate) {
        apply_skill_policies(candidate, changes);
        return true;
    });
}

void ProviderStore::remember_dialect(
    std::string_view connection_id, std::string_view model, ApiStandard dialect)
{
    std::ignore = _update_config([&](Config& candidate) {
        auto* connection = const_cast<Connection*>(
            find_connection(candidate.providers, connection_id));
        if (connection == nullptr) {
            return false;
        }
        connection->dialects[std::string(model)] = dialect;
        return true;
    });
}

void ProviderStore::ensure_catalog_fresh()
{
    {
        std::lock_guard lock(_mutex);
        if (_catalog_syncing || !catalog_stale(_catalog)) {
            return;
        }
        _catalog_syncing = true;
    }
    _catalog_worker.emplace([this] {
        Catalog catalog;
        const Status status = fetch_catalog(catalog);
        if (!alive_.load()) {
            return;
        }
        {
            std::lock_guard lock(_mutex);
            _catalog_syncing = false;
            if (status != Status::OK) {
                return;
            }
            _catalog = catalog;
            save_catalog(presets_path(), _catalog);
            _pricing = pricing_table_from(_catalog);
        }
        _notify_changed();
    });
}

bool ProviderStore::_update_config(
    std::function<bool(Config&)> mutate, std::function<void()> on_commit)
{
    bool committed = false;
    {
        std::lock_guard lock(_mutex);
        Config candidate;
        const ConfigUpdateResult result
            = update_config(config_path(), _config, mutate, &candidate);
        if (result != ConfigUpdateResult::FAILURE) {
            _config = std::move(candidate);
        }
        if (result == ConfigUpdateResult::UPDATED) {
            committed = true;
            if (on_commit) {
                on_commit();
            }
        }
    }
    if (committed) {
        _notify_changed();
    }
    return committed;
}

Connection* ProviderStore::_find_locked(std::string_view id)
{
    return const_cast<Connection*>(
        static_cast<const ProviderStore*>(this)->_find_locked(id));
}

const Connection* ProviderStore::_find_locked(std::string_view id) const
{
    return find_connection(_config.providers, id);
}

Route ProviderStore::_route_locked(const Connection& connection,
    ApiStandard dialect, std::string_view opencode_session) const
{
    return resolve_route(connection, _catalog, dialect, opencode_session);
}

void ProviderStore::_start_fetch_locked(const std::string& connection_id)
{
    const Connection* connection = _find_locked(connection_id);
    if (connection == nullptr) {
        return;
    }
    const Route route
        = _route_locked(*connection, default_dialect(*connection, _catalog));
    const int generation = ++_generations[connection_id];
    if (route.api.empty()) {
        _model_catalog[connection_id]
            = CatalogEntry { CatalogEntry::Failed { Status::INVALID_URL } };
        return;
    }
    _model_catalog[connection_id] = CatalogEntry { CatalogEntry::Fetching { } };
    if (subscription_connection(connection->id)) {
        _model_catalog[connection_id] = CatalogEntry { CatalogEntry::Ready {
            catalog_models(_catalog, connection->id) } };
        return;
    }
    _workers.emplace_back([this, connection_id, generation, route] {
        std::vector<ModelInfo> models;
        const Status status = _models_fn(route, models);
        if (!alive_.load()) {
            return;
        }
        {
            std::lock_guard lock(_mutex);
            if (_generations[connection_id] != generation) {
                return;
            }
            if (status == Status::OK) {
                _model_catalog[connection_id] = CatalogEntry {
                    CatalogEntry::Ready { std::move(models) }
                };
            } else {
                _model_catalog[connection_id]
                    = CatalogEntry { CatalogEntry::Failed { status } };
            }
        }
        _notify_changed();
    });
}

Status ProviderStore::_commit_connection_locked(const ConnectResult& result,
    const std::vector<ModelInfo>& models, bool& first)
{
    Connection stored;
    stored.api_key       = result.api_key;
    stored.refresh_token = result.refresh_token;
    stored.expires_at    = result.expires_at;
    stored.account_id    = result.account_id;
    stored.label         = result.label;
    if (!result.endpoint.empty()) {
        stored.endpoint = result.endpoint;
    }

    std::string id;
    Config candidate;
    const ConfigUpdateResult updated = update_config(
        config_path(), _config,
        [&](Config& latest) {
            first     = !latest.last_used.has_value();
            stored.id = result.id;
            id        = connection_key(stored);
            for (const Connection& connection : latest.providers) {
                if (connection_key(connection) == id) {
                    return false;
                }
            }
            latest.providers.push_back(std::move(stored));
            return true;
        },
        &candidate);
    if (updated != ConfigUpdateResult::UPDATED) {
        return Status::CONFIG_ERROR;
    }
    _config = std::move(candidate);
    ++_generations[id];
    _model_catalog[id] = CatalogEntry { CatalogEntry::Ready { models } };
    return Status::OK;
}

void ProviderStore::_notify_changed() { changed_.publish(); }

} // namespace imza
