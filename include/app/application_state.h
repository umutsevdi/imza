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
#include "platform/config_store.h"
#include "providers/store.h"
#include "runtime/bg_process_subsystem.h"
#include "runtime/callbacks.h"
#include "runtime/main_thread_queue.h"
#include "runtime/notification_center.h"
#include "runtime/subagent_subsystem.h"
#include "tools/lua.h"
#include "tools/skill_subsystem.h"
#include "tools/tool.h"
#include "workspace/environment.h"

namespace imza {

class ReviewSubsystem;
class TurnRunner;
class Delegation;
class PermissionStore;
class PromptStore;
class McpSubsystem;
class SkillSubsystem;

struct ApplicationState {
    std::shared_ptr<Session> session;
    std::shared_ptr<SessionStore> sessions;
    std::shared_ptr<InputHistoryStore> input_history;
    // The single owner of the persisted Config document; every
    // writer goes through it, so no reader can hold a stale copy.
    std::shared_ptr<ConfigStore> config;
    std::shared_ptr<ProviderStore> providers;
    std::shared_ptr<McpSubsystem> mcp;
    std::shared_ptr<SubagentSubsystem> subagents;
    std::shared_ptr<BgProcessSubsystem> bg_processes;
    std::shared_ptr<Environment> environment;
    std::shared_ptr<ReviewSubsystem> review;
    std::shared_ptr<SkillSubsystem> skill_subsystem;
    std::shared_ptr<PermissionStore> permissions;
    std::shared_ptr<PromptStore> prompts;
    // App-wide transient messages; shared with children and the sidechat so
    // one overlay shows every publisher's notice.
    std::shared_ptr<NotificationStore> notifications;

    // The main-thread task queue. Created by the factory so the state owns
    // it; the interactive loop and the headless drain read it here instead
    // of threading a separate reference through every entry point.
    std::shared_ptr<MainThreadQueue> main_thread;

    std::unique_ptr<LuaState> lua_state;
    std::unique_ptr<TurnRunner> runner;
    std::unique_ptr<Delegation> delegation;
    // Lazy accessor the subagent tool uses to reach this state's
    // delegation; wire() fills it once the delegation exists.
    SubagentDeps subagent_deps;

    // Sidechat pane, declared last so it dies before the components it shares.
    std::shared_ptr<ApplicationState> sidechat;
    bool sidechat_open             = false;
    bool sidechat_context_seeded   = false;
    ApplicationState* parent_state = nullptr;

    PostFn post;
    std::function<void()> on_exit;
    ModalRequestFn parent_routing;
    std::string agent_label;
    RuntimeFlag runtime_flags = interactive_runtime_flags();
    // Sidebar widget visibility, captured from the config at startup and
    // inherited by children; presentation state, not a provider concern.
    SidebarConfig sidebar;
    std::atomic<bool> alive { true };
    Signal<>::Subscription env_subscription;
    Signal<>::Subscription system_subscription;
    Signal<>::Subscription provider_subscription;
    Signal<>::Subscription update_subscription;

    // Components with teardown work, registered by the factories in the
    // order they must stop. The destructor marks each dead and stops it in
    // reverse; adding a worker-bearing component means registering it, not
    // editing the destructor.
    std::vector<Subsystem*> stoppable;

    ~ApplicationState();

    ApplicationState(const ApplicationState&)            = delete;
    ApplicationState& operator=(const ApplicationState&) = delete;

private:
    ApplicationState() = default;
    friend std::shared_ptr<ApplicationState> make_application_state(
        PostFn, Config, StreamFn, RuntimeFlag);
    friend std::shared_ptr<ApplicationState> make_child_application_state(
        const ApplicationState&, PostFn, StreamFn, ModalRequestFn, std::string);
    friend std::shared_ptr<ApplicationState> make_sidechat_application_state(
        ApplicationState&);
};

std::shared_ptr<ApplicationState> make_application_state(PostFn post,
    Config config, StreamFn stream_fn = { },
    RuntimeFlag runtime_flags = interactive_runtime_flags());

std::shared_ptr<ApplicationState> make_child_application_state(
    const ApplicationState& parent, PostFn post, StreamFn stream_fn = { },
    ModalRequestFn parent_routing = { }, std::string agent_label = { });

std::shared_ptr<ApplicationState> make_sidechat_application_state(
    ApplicationState& parent);

} // namespace imza
