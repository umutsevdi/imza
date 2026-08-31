#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "catalog.h"
#include "network.h"
#include "types.h"
#include "ursa_signal.h"

namespace ursa {

using ModelsFn = std::function<Status(const Route&, std::vector<ModelInfo>&)>;
using ProviderChangedFn = std::function<void()>;

struct ConnectionView {
    enum class State { FETCHING, READY, FAILED };
    std::string id;
    std::string provider_id;
    std::string name;
    bool active = false;
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

struct ConnectOutcome {
    Status status           = Status::OK;
    std::size_t model_count = 0;
    bool first_connection   = false;
    bool persisted          = false;
};

using ConnectCompleteFn = std::function<void(ConnectOutcome)>;

class ProviderStore {
public:
    explicit ProviderStore(Config config, ModelsFn models_fn = { });
    ~ProviderStore();

    ProviderStore(const ProviderStore&)            = delete;
    ProviderStore& operator=(const ProviderStore&) = delete;

    [[nodiscard]] Signal<>::Subscription subscribe(ProviderChangedFn callback);

    Config config() const;
    StatusConfigView status() const;
    std::vector<ConnectionView> connections() const;
    ModelList models_for(std::string_view connection_id) const;
    std::vector<std::pair<std::string, std::string>> provider_options() const;
    std::optional<ProviderSelection> active_selection() const;
    std::optional<ProviderSelection> subagent_selection(
        SubagentRole role) const;
    Route route_for(std::string_view connection_id, ApiStandard dialect) const;
    bool model_reasons(std::string_view model) const;

    void start_model_fetches();
    void refetch_models(std::string_view connection_id);
    void connect(ConnectResult result, ConnectCompleteFn complete);
    bool remove_connection(std::string_view connection_id);
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

    Connection* _find_locked(std::string_view id);
    const Connection* _find_locked(std::string_view id) const;
    std::string _unique_id_locked(std::string base) const;
    Route _route_locked(
        const Connection& connection, ApiStandard dialect) const;
    void _start_fetch_locked(const std::string& connection_id);
    Status _commit_connection_locked(const ConnectResult& result,
        const std::vector<ModelInfo>& models, bool& first);
    bool _update_config(std::function<bool(Config&)> mutate,
        std::function<void()> on_commit = { });
    void _notify_changed();

    Config config_;
    Catalog catalog_;
    ModelsFn models_fn_;

    std::map<std::string, CatalogEntry> model_catalog_;
    std::map<std::string, int> generations_;
    bool catalog_syncing_ = false;
    mutable std::mutex mutex_;
    std::vector<std::jthread> workers_;
    std::optional<std::jthread> catalog_worker_;
    Signal<> changed_;
    std::atomic<bool> alive_ { true };
};

} // namespace ursa
