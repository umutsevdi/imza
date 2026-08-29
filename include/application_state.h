#pragma once

#include <memory>

namespace ursa {

class Environment;
class ProviderStore;
class Session;
class SubagentManager;

struct ApplicationState {
    std::shared_ptr<Session> session;
    std::shared_ptr<ProviderStore> providers;
    std::shared_ptr<SubagentManager> subagents;
    std::shared_ptr<Environment> environment;
};

} // namespace ursa
