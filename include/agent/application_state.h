#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "agent/modal_queue.h"
#include "agent/subsystems/session.h"
#include "agent/subsystems/subagent_manager.h"
#include "common/tool_call.h"
#include "core/config.h"
#include "environment/environment.h"
#include "network/network.h"
#include "provider/provider_store.h"
#include "common/types.h"
#include "common/ursa_signal.h"

namespace ursa {

class ReviewState;
class SkillStore;
class TurnRunner;
class DelegationRunner;

using PostFn = std::function<void(std::function<void()>)>;
using StreamFn
    = std::function<Status(const ChatRequest&, const StreamCallback&)>;
using ModalRequestFn = std::function<std::future<ModalResult>(ModalPayload)>;

struct ApplicationState {
    std::shared_ptr<Session> session;
    std::shared_ptr<ProviderStore> providers;
    std::shared_ptr<SubagentManager> subagents;
    std::shared_ptr<Environment> environment;
    std::shared_ptr<ReviewState> review;
    std::shared_ptr<SkillStore> skills;

    std::unique_ptr<TurnRunner> runner;
    std::unique_ptr<DelegationRunner> delegation;
    ModalQueue queue;

    PostFn post;
    std::function<void()> on_exit;
    ModalRequestFn parent_routing;
    std::string agent_label;
    std::atomic<bool> alive { true };
    Signal<>::Subscription env_subscription;
    Signal<>::Subscription provider_subscription;

    ~ApplicationState();

    ApplicationState(const ApplicationState&)            = delete;
    ApplicationState& operator=(const ApplicationState&) = delete;

private:
    ApplicationState() = default;
    friend std::shared_ptr<ApplicationState> make_application_state(PostFn,
        Config, StreamFn, std::vector<Tool>, ModalRequestFn, std::string);
    friend std::shared_ptr<ApplicationState> make_child_application_state(
        const ApplicationState&, PostFn, StreamFn, std::vector<Tool>,
        ModalRequestFn, std::string);
};

std::shared_ptr<ApplicationState> make_application_state(PostFn post,
    Config config, StreamFn stream_fn = { }, std::vector<Tool> tools = { },
    ModalRequestFn parent_routing = { }, std::string agent_label = { });

std::shared_ptr<ApplicationState> make_child_application_state(
    const ApplicationState& parent, PostFn post, StreamFn stream_fn = { },
    std::vector<Tool> tools = { }, ModalRequestFn parent_routing = { },
    std::string agent_label = { });

} // namespace ursa
