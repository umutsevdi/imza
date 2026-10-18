#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "common/imza_signal.h"
#include "common/types.h"
#include "network/network.h"
#include "platform/config.h"
#include "platform/config_store.h"
#include "providers/catalog.h"
#include "providers/pricing.h"
#include "providers/subscriptions.h"

namespace imza {

std::string subagent_variant_or_default(
    const SubagentModelConfig* configured, SubagentRole role);

using ModelsFn = std::function<Status(const Route&, std::vector<ModelInfo>&)>;
// Accepts a provider's terms of service before its connect probe; only
// providers that require it are passed through here.
using TermsFn           = std::function<Status(const Route&)>;
using ProviderChangedFn = std::function<void()>;
// Subscription token refresh behind authenticated_route_for; injectable so
// the refresh boundary is testable without network.
using RefreshFn
    = std::function<SubscriptionResult(std::string_view connection_id,
        std::string_view refresh_token, std::string_view account_id)>;

struct ConnectionView {
    enum class State { FETCHING, READY, FAILED };
    std::string id;
    std::string provider;
    std::string name;
    std::string api_key;
    State state             = State::FETCHING;
    Status error            = Status::OK;
    std::size_t model_count = 0;
};

struct ModelList {
    enum class State { FETCHING, READY, FAILED };
    State state  = State::FETCHING;
    Status error = Status::OK;
    std::vector<ModelInfo> models;
};

struct StatusConfigView {
    std::string active_model;
    std::string reasoning_effort;
};

struct ProviderSelection {
    std::string model;
    std::string reasoning_effort;
    std::string connection_id;
    Route route;
};

// One model's facts, resolved from the connection's live fetch first (a
// provider may serve its own variant with different limits), then its
// models.dev catalog row. Absent means unknown, never borrowed from another
// provider.
struct ModelMetadata {
    std::uint64_t context_limit = 0;
    std::optional<Capabilities> capabilities;
    bool reasoning = false;
};

struct ConnectOutcome {
    Status status           = Status::OK;
    std::size_t model_count = 0;
    bool first_connection   = false;
    bool persisted          = false;
};

using ConnectCompleteFn = std::function<void(ConnectOutcome)>;

class ProviderStore final : public Subsystem {
public:
    explicit ProviderStore(std::shared_ptr<ConfigStore> config_store,
        ModelsFn models_fn = { }, TermsFn terms_fn = { },
        RefreshFn refresh_fn = { });
    ~ProviderStore();

    ProviderStore(const ProviderStore&)            = delete;
    ProviderStore& operator=(const ProviderStore&) = delete;

    [[nodiscard]] Signal<>::Subscription subscribe_to_change(
        ProviderChangedFn callback);

    Config config() const;
    // The store every writer shares; the composition root keeps the
    // ConfigStore alive for the process lifetime.
    ConfigStore& config_store() const { return *_config_store; }
    StatusConfigView status() const;
    std::vector<ConnectionView> connections() const;
    ModelList models_for(std::string_view connection_id) const;
    std::vector<std::pair<std::string, std::string>> provider_options() const;
    std::optional<ProviderSelection> active_selection() const;
    std::optional<ProviderSelection> subagent_selection(
        SubagentRole role) const;
    Route route_for(std::string_view connection_id, ApiStandard dialect) const;
    Route authenticated_route_for(std::string_view connection_id,
        ApiStandard dialect, std::string_view opencode_session = { });
    ModelMetadata model_metadata_for(
        std::string_view model, std::string_view connection_id) const;
    // The model's context window for a connection; 0 when unknown.
    std::uint64_t context_limit_for(
        std::string_view model, std::string_view connection_id) const;
    bool model_reasons(
        std::string_view model, std::string_view connection_id) const;
    // Cost row for one connection's model; zero when it has no catalog row.
    ModelPricing pricing_for(
        std::string_view model, std::string_view connection_id) const;

    void start_model_fetches();
    void refetch_models(std::string_view connection_id);
    void connect(ConnectResult result, ConnectCompleteFn complete);
    bool remove_connection(std::size_t index, std::string_view expected_id);
    bool select_model(const ModelChoice& choice);
    bool set_reasoning_effort(std::string effort);
    bool set_subagent_model(SubagentRole role, SubagentModelConfig selection);
    bool set_skill_policies(const SkillPolicyChanges& changes);
    void remember_dialect(std::string_view connection_id,
        std::string_view model, ApiStandard dialect);
    void ensure_catalog_fresh();

private:
    struct CatalogEntry {
        struct Fetching { };
        struct Ready {
            std::vector<ModelInfo> models;
        };
        struct Failed {
            Status status = Status::OK;
        };
        std::variant<Fetching, Ready, Failed> state;
    };

    Route _route_locked(const Connection& connection, ApiStandard dialect,
        std::string_view opencode_session = { }) const;
    void _load_catalog();
    void _maybe_start_catalog_refresh();
    void _wait_catalog_locked(std::unique_lock<std::mutex>& lock) const;
    ProviderSelection _selection_locked(const Connection& connection,
        const std::string& model, std::string variant) const;
    void _start_fetch_locked(const std::string& connection_id);
    Status _commit_connection_locked(const ConnectResult& result,
        const std::vector<ModelInfo>& models, bool& first);
    bool _update_config(
        const ConfigMutator& mutate, std::function<void()> on_commit = { });
    void _notify_changed();

    std::shared_ptr<ConfigStore> _config_store;
    Catalog _catalog;
    std::map<std::string, ModelPricing> _pricing;
    ModelsFn _models_fn;
    TermsFn _terms_fn;
    RefreshFn _refresh_fn;

    std::map<std::string, CatalogEntry> _model_catalog;
    std::map<std::string, int> _generations;
    bool _catalog_syncing         = false;
    mutable bool _catalog_loaded  = false;
    bool _catalog_refresh_pending = false;
    mutable std::condition_variable _catalog_cv;
    std::optional<std::jthread> _catalog_loader;
    mutable std::mutex _mutex;
    std::condition_variable _refresh_changed;
    std::set<std::string> _refreshing;
    std::vector<std::jthread> _workers;
    std::optional<std::jthread> _catalog_worker;
    Signal<> _changed;
};

} // namespace imza
