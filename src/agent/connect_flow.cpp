#include "controller.h"

#include <algorithm>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ursa {

Config Controller::config() const
{
    std::lock_guard lock(data_mutex_);
    return cfg_;
}

std::vector<ConnectionView> Controller::connections() const
{
    std::lock_guard lock(data_mutex_);
    std::vector<ConnectionView> views;
    views.reserve(cfg_.providers.size());
    for (const Connection& conn : cfg_.providers) {
        ConnectionView view;
        view.id          = conn.id;
        view.provider_id = conn.provider_id;
        view.api_key     = conn.api_key;
        view.active = cfg_.last_used && cfg_.last_used->provider == conn.id;
        if (conn.provider_id == kLocalProviderId) {
            view.name = "Local";
        } else if (conn.provider_id == kCustomProviderId) {
            view.name = "Custom";
        } else if (const auto it = catalog_.providers.find(conn.provider_id);
            it != catalog_.providers.end()) {
            view.name = it->second.name;
        }
        if (view.name.empty()) {
            view.name = conn.provider_id;
        }
        const auto it = model_catalog_.find(conn.id);
        if (it != model_catalog_.end()) {
            if (auto* ready
                = std::get_if<CatalogEntry::Ready>(&it->second.state)) {
                view.state       = ConnectionView::State::READY;
                view.model_count = ready->models.size();
            } else if (auto* failed
                = std::get_if<CatalogEntry::Failed>(&it->second.state)) {
                view.state = ConnectionView::State::FAILED;
                view.error = failed->status;
            }
        }
        views.push_back(std::move(view));
    }
    return views;
}

ModelList Controller::models_for(const std::string& connection_id) const
{
    std::lock_guard lock(data_mutex_);
    ModelList list;
    const Connection* conn = nullptr;
    for (const Connection& conn_item : cfg_.providers) {
        if (conn_item.id == connection_id) {
            conn = &conn_item;
            break;
        }
    }
    if (conn == nullptr) {
        list.state = ModelList::State::FAILED;
        list.error = Status::CONFIG_ERROR;
        return list;
    }
    const auto it = model_catalog_.find(connection_id);
    if (it == model_catalog_.end()) {
        return list;
    }
    auto* ready = std::get_if<CatalogEntry::Ready>(&it->second.state);
    if (ready == nullptr) {
        if (auto* failed
            = std::get_if<CatalogEntry::Failed>(&it->second.state)) {
            list.state = ModelList::State::FAILED;
            list.error = failed->status;
        }
        return list;
    }
    list.state          = ModelList::State::READY;
    list.models         = ready->models;
    const auto provider = catalog_.providers.find(conn->provider_id);
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

bool Controller::remove_connection(const std::string& connection_id)
{
    bool removed = false;
    {
        std::lock_guard lock(data_mutex_);
        if (cfg_.providers.size() <= 1) {
            return false;
        }
        auto& providers = cfg_.providers;
        providers.erase(std::remove_if(providers.begin(), providers.end(),
                            [&](const Connection& conn) {
                                return conn.id == connection_id;
                            }),
            providers.end());
        removed = true;
        ++generations_[connection_id];
        model_catalog_.erase(connection_id);
        if (cfg_.last_used && cfg_.last_used->provider == connection_id) {
            if (!cfg_.providers.empty()) {
                cfg_.last_used = LastUsed { cfg_.providers.front().id, "" };
            } else {
                cfg_.last_used.reset();
            }
        }
        save_config(config_path(), cfg_);
    }
    if (removed) {
        session_->bump_modal_serial();
    }
    return removed;
}

void Controller::refetch_models(const std::string& connection_id)
{
    std::lock_guard lock(data_mutex_);
    _start_fetch_locked(connection_id);
}

std::vector<std::pair<std::string, std::string>>
Controller::provider_options() const
{
    std::lock_guard lock(data_mutex_);
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

Route Controller::_active_route_locked(const std::string& model) const
{
    if (!cfg_.last_used) {
        return Route { };
    }
    for (const Connection& conn : cfg_.providers) {
        if (conn.id != cfg_.last_used->provider) {
            continue;
        }
        ApiStandard dialect = ApiStandard::OPENAI;
        if (const auto it = conn.dialects.find(model);
            it != conn.dialects.end()) {
            dialect = it->second;
        }
        return resolve_route(conn, catalog_, dialect);
    }
    return Route { };
}

std::string Controller::_unique_id_locked(std::string base) const
{
    const auto taken = [&](const std::string& id) {
        for (const Connection& conn : cfg_.providers) {
            if (conn.id == id) {
                return true;
            }
        }
        return false;
    };
    if (!taken(base)) {
        return base;
    }
    for (int n = 2;; ++n) {
        std::string candidate = base + "-" + std::to_string(n);
        if (!taken(candidate)) {
            return candidate;
        }
    }
}

Connection* Controller::_find_locked(const std::string& id)
{
    for (Connection& conn : cfg_.providers) {
        if (conn.id == id) {
            return &conn;
        }
    }
    return nullptr;
}

void Controller::_start_fetch_locked(const std::string& connection_id)
{
    const Connection* conn = _find_locked(connection_id);
    if (conn == nullptr) {
        return;
    }
    const Route route = resolve_route(*conn, catalog_, ApiStandard::OPENAI);
    const int gen     = ++generations_[connection_id];
    if (route.api.empty()) {
        model_catalog_[connection_id]
            = CatalogEntry { CatalogEntry::Failed { Status::INVALID_URL } };
        return;
    }
    model_catalog_[connection_id] = CatalogEntry { CatalogEntry::Fetching { } };

    auto models = std::make_shared<std::vector<ModelInfo>>();
    ModelsFn fn = models_fn_;
    if (!fn) {
        fn = [](const Route& r, std::vector<ModelInfo>& out) {
            return fetch_models(r, out);
        };
    }
    std::shared_future<Status> future
        = std::async(std::launch::async, [fn, route, models] {
              return fn(route, *models);
          }).share();
    fetch_threads_.emplace_back([this, connection_id, gen, future, models] {
        future.wait();
        if (!alive_.load()) {
            return;
        }
        _post([this, connection_id, gen, future, models] {
            std::lock_guard lock(data_mutex_);
            if (generations_[connection_id] != gen) {
                return;
            }
            const Status st = future.get();
            if (st == Status::OK) {
                model_catalog_[connection_id]
                    = CatalogEntry { CatalogEntry::Ready { *models } };
            } else {
                model_catalog_[connection_id]
                    = CatalogEntry { CatalogEntry::Failed { st } };
            }
        });
    });
}

void Controller::_begin_connect(const ConnectResult& res)
{
    Route route;
    bool known = false;
    {
        std::lock_guard lock(data_mutex_);
        known = res.provider_id == kLocalProviderId
            || res.provider_id == kCustomProviderId
            || catalog_.providers.count(res.provider_id) > 0;
        if (known) {
            Connection probe;
            probe.provider_id = res.provider_id;
            probe.endpoint    = res.endpoint;
            probe.api_key     = res.api_key;
            route = resolve_route(probe, catalog_, ApiStandard::OPENAI);
        }
    }
    if (!known || route.endpoint.empty()) {
        session_->set_connect_status("unknown provider");
        return;
    }

    auto models = std::make_shared<std::vector<ModelInfo>>();
    ModelsFn fn = models_fn_;
    if (!fn) {
        fn = [](const Route& r, std::vector<ModelInfo>& out) {
            return fetch_models(r, out);
        };
    }
    std::shared_future<Status> future
        = std::async(std::launch::async, [fn, route, models] {
              return fn(route, *models);
          }).share();
    fetch_threads_.emplace_back([this, res, future, models] {
        future.wait();
        if (!alive_.load()) {
            return;
        }
        _post([this, res, future, models] {
            const Status st = future.get();
            if (st != Status::OK) {
                session_->set_connect_status(error_text(st));
                return;
            }
            session_->set_connect_status(
                "✓ " + std::to_string(models->size()) + " models");
            if (res.persist) {
                const bool first = _commit_connection(res);
                if (std::holds_alternative<ConnectModal>(session_->modal())) {
                    session_->set_modal(
                        ConnectModal { first ? ConnectModal::Entry::PICK_MODEL
                                             : ConnectModal::Entry::MANAGE });
                    session_->bump_modal_serial();
                }
            }
        });
    });
}

bool Controller::_commit_connection(const ConnectResult& res)
{
    std::lock_guard lock(data_mutex_);
    const bool first = !cfg_.last_used.has_value();

    Connection probe;
    probe.provider_id = res.provider_id;
    probe.endpoint    = res.endpoint;
    probe.api_key     = res.api_key;
    const Route route = resolve_route(probe, catalog_, ApiStandard::OPENAI);

    Connection stored;
    stored.provider_id = res.provider_id;
    stored.api_key     = res.api_key;
    if (res.provider_id == kLocalProviderId
        || res.provider_id == kCustomProviderId) {
        stored.endpoint = res.endpoint;
    }

    Connection* existing = nullptr;
    if (!route.endpoint.empty()) {
        for (Connection& conn : cfg_.providers) {
            const Route other
                = resolve_route(conn, catalog_, ApiStandard::OPENAI);
            if (other.endpoint == route.endpoint) {
                existing = &conn;
                break;
            }
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
        stored.id = _unique_id_locked(res.provider_id);
        id        = stored.id;
        cfg_.providers.push_back(std::move(stored));
    }
    save_config(config_path(), cfg_);
    _start_fetch_locked(id);
    return first;
}

void Controller::_apply_pick(const ModelChoice& choice)
{
    std::lock_guard lock(data_mutex_);
    if (_find_locked(choice.connection_id) == nullptr) {
        return;
    }
    cfg_.last_used = LastUsed { choice.connection_id, choice.model_id };
    save_config(config_path(), cfg_);
}

} // namespace ursa