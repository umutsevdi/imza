#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "common/imza_signal.h"
#include "common/types.h"
#include "conversation/input_history.h"
#include "conversation/session.h"
#include "conversation/session_store.h"
#include "network/network.h"
#include "platform/config.h"
#include "providers/store.h"
#include "runtime/modal_queue.h"
#include "runtime/subagent_manager.h"
#include "tools/tool.h"
#include "workspace/environment.h"

namespace imza {

class ReviewState;
class SkillStore;
class TurnRunner;
class Delegation;
class PermissionStore;
class PromptStore;

enum class AgentNotification { TURN_FINISHED, INPUT_REQUIRED };

using PostFn = std::function<void(std::function<void()>)>;
using StreamFn
    = std::function<Status(const ChatRequest&, const StreamCallback&)>;
using ModalRequestFn = std::function<std::future<ModalResult>(ModalPayload)>;

struct ApplicationState {
    std::shared_ptr<Session> session;
    std::shared_ptr<SessionStore> sessions;
    std::shared_ptr<InputHistoryStore> input_history;
    std::shared_ptr<ProviderStore> providers;
    std::shared_ptr<SubagentManager> subagents;
    std::shared_ptr<Environment> environment;
    std::shared_ptr<ReviewState> review;
    std::shared_ptr<SkillStore> skills;
    std::shared_ptr<PermissionStore> permissions;
    std::shared_ptr<PromptStore> prompts;

    std::unique_ptr<TurnRunner> runner;
    std::unique_ptr<Delegation> delegation;
    // Shared with the subagent tool; wire() fills it once both exist.
    SubagentToolSlot subagent_slot;
    ModalQueue queue;

    // Sidechat pane, declared last so it dies before the components it shares.
    std::shared_ptr<ApplicationState> sidechat;
    bool sidechat_open             = false;
    bool sidechat_context_seeded   = false;
    ApplicationState* parent_state = nullptr;

    PostFn post;
    std::function<void()> on_exit;
    std::function<void(AgentNotification)> notify_user;
    ModalRequestFn parent_routing;
    std::string agent_label;
    RuntimeFlag runtime_flags = interactive_runtime_flags();
    std::atomic<bool> alive { true };
    Signal<>::Subscription env_subscription;
    Signal<>::Subscription provider_subscription;

    ~ApplicationState();

    ApplicationState(const ApplicationState&)            = delete;
    ApplicationState& operator=(const ApplicationState&) = delete;

private:
    ApplicationState() = default;
    friend std::shared_ptr<ApplicationState> make_application_state(
        PostFn, Config, StreamFn, RuntimeFlag);
    friend std::shared_ptr<ApplicationState> make_application_state_with_tools(
        PostFn, Config, std::vector<Tool>, StreamFn, RuntimeFlag,
        SubagentToolSlot);
    friend std::shared_ptr<ApplicationState> make_child_application_state(
        const ApplicationState&, PostFn, StreamFn, ModalRequestFn, std::string);
    friend std::shared_ptr<ApplicationState> make_sidechat_application_state(
        ApplicationState&);
};

std::shared_ptr<ApplicationState> make_application_state(PostFn post,
    Config config, StreamFn stream_fn = { },
    RuntimeFlag runtime_flags = interactive_runtime_flags());
std::shared_ptr<ApplicationState> make_application_state_with_tools(PostFn post,
    Config config, std::vector<Tool> tools, StreamFn stream_fn = { },
    RuntimeFlag runtime_flags      = interactive_runtime_flags(),
    SubagentToolSlot subagent_slot = nullptr);

std::shared_ptr<ApplicationState> make_child_application_state(
    const ApplicationState& parent, PostFn post, StreamFn stream_fn = { },
    ModalRequestFn parent_routing = { }, std::string agent_label = { });

std::shared_ptr<ApplicationState> make_sidechat_application_state(
    ApplicationState& parent);

} // namespace imza



