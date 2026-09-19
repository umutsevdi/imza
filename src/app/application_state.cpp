#include "app/application_state.h"

#include "app/flows.h"
#include "permissions/evaluator.h"
#include "permissions/filesystem.h"
#include "permissions/store.h"
#include "tools/skills.h"
#include "turn/delegation.h"
#include "turn/prompt.h"
#include "turn/turn_runner.h"
#include "workspace/review.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <utility>

namespace imza {

namespace {

    PostFn guarded_post(ApplicationState* state, PostFn post)
    {
        return [state, post = std::move(post)](std::function<void()> f) {
            if (state->alive.load()) {
                post(std::move(f));
            }
        };
    }

    // Lua tool bindings reach session state, the modal queue, and the
    // skill catalog through this host; the ask routes into the modal queue
    // only in attended mode.
    LuaHost lua_host(ApplicationState* state)
    {
        return LuaHost {
            .context =
                [state] {
                    return state->environment && state->permissions
                        ? permission_context(*state->environment,
                              *state->permissions, state->session->mode())
                        : PermissionContext { };
                },
            .ask = [state](ModalPayload payload) -> std::future<ModalResult> {
                if ((state->runtime_flags & RuntimeFlag::ATTENDED)
                    == RuntimeFlag::NONE) {
                    std::promise<ModalResult> denied;
                    denied.set_value(
                        ToolVerdict { ToolDecision::REJECT, "unattended run" });
                    return denied.get_future();
                }
                return request_modal(*state, std::move(payload));
            },
            .todo = [state] { return state->session->todo(); },
            .set_todo
            = [state](
                  TodoList todo) { state->session->set_todo(std::move(todo)); },
            .skills = [state] { return state->environment->skills(); },
            .config = [state] { return state->providers->config(); },
            .skill_store = [state]() -> SkillStore& { return *state->skills; },
            .web_enabled
            = (state->runtime_flags & RuntimeFlag::WEB) != RuntimeFlag::NONE,
        };
    }

    void wire(std::shared_ptr<ApplicationState> state, StreamFn stream_fn,
        std::vector<Tool> tools)
    {
        ApplicationState* raw = state.get();
        state->runner         = std::make_unique<TurnRunner>(
            *raw, state->post, std::move(tools), std::move(stream_fn),
            [raw](ModalPayload payload) {
                return request_modal(*raw, std::move(payload));
            },
            state->skills, SubagentToolFn { },
            std::function<void(std::string)> { });
        state->delegation = std::make_unique<Delegation>(
            *raw, state->post,
            [raw](ModalPayload payload) {
                return request_modal(*raw, std::move(payload));
            },
            *state->runner);
        state->runner->set_subagent_tool(
            [raw](const ToolCallRequest& req, std::vector<Message>& msgs) {
                raw->delegation->run_subagents(req, msgs);
            });
        state->runner->set_on_finish([raw](std::string error) {
            on_turn_finished(*raw, std::move(error));
        });
        state->env_subscription
            = state->environment->subscribe_to_workspace_change([raw] {
                  raw->post([raw] {
                      present_front(*raw);
                      if (raw->session->phase() == Session::Phase::IDLE
                          && !raw->session->queued().empty()) {
                          drain_queued(*raw);
                      }
                  });
              });
        state->provider_subscription = state->providers->subscribe(
            [raw] { raw->post([raw] { raw->session->bump_modal_serial(); }); });
        state->post([raw] { raw->providers->start_model_fetches(); });
    }

} // namespace

namespace {

    std::shared_ptr<ApplicationState> initialize_root(
        std::shared_ptr<ApplicationState> state, PostFn post, Config config,
        StreamFn stream_fn, std::vector<Tool> tools, RuntimeFlag runtime_flags,
        bool use_default_tools)
    {
        state->prompts  = std::make_shared<PromptStore>(prompts_dir());
        state->session  = std::make_shared<Session>();
        state->sessions = std::make_shared<SessionStore>();
        state->input_history
            = std::make_shared<InputHistoryStore>(input_history_path());
        state->providers   = std::make_shared<ProviderStore>(std::move(config));
        state->subagents   = std::make_shared<SubagentManager>();
        state->environment = std::make_shared<Environment>();
        state->review      = std::make_shared<ReviewState>();
        state->skills      = std::make_shared<SkillStore>();
        state->permissions = std::make_shared<PermissionStore>();
        state->post        = guarded_post(state.get(), std::move(post));
        state->on_exit     = [] { };
        state->runtime_flags = runtime_flags;
        if (use_default_tools) {
            ApplicationState* captured = state.get();
            tools                      = default_tools(runtime_flags,
                state->environment->system()->has_rg, lua_host(captured));
        }
        wire(state, std::move(stream_fn), std::move(tools));
        return state;
    }

    std::shared_ptr<ApplicationState> initialize_child(
        std::shared_ptr<ApplicationState> state, const ApplicationState& parent,
        PostFn post, StreamFn stream_fn, ModalRequestFn parent_routing,
        std::string agent_label, std::vector<Tool> tools)
    {
        state->session        = std::make_shared<Session>();
        state->sessions       = parent.sessions;
        state->input_history  = parent.input_history;
        state->providers      = parent.providers;
        state->subagents      = std::make_shared<SubagentManager>();
        state->environment    = parent.environment;
        state->review         = std::make_shared<ReviewState>();
        state->skills         = std::make_shared<SkillStore>();
        state->permissions    = parent.permissions;
        state->prompts        = parent.prompts;
        state->post           = guarded_post(state.get(), std::move(post));
        state->on_exit        = [] { };
        state->parent_routing = std::move(parent_routing);
        state->agent_label    = std::move(agent_label);
        state->runtime_flags  = parent.runtime_flags;
        wire(state, std::move(stream_fn), std::move(tools));
        return state;
    }

    std::vector<Tool> sidechat_roster(ApplicationState& parent)
    {
        std::vector<Tool> tools = default_tools(parent.runtime_flags,
            parent.environment->system()->has_rg, lua_host(&parent));
        // The sidechat is a regular chat; drop only file mutation and
        // delegation.
        std::erase_if(tools, [](const Tool& tool) {
            constexpr std::string_view removed[]
                = { "edit", "write", "subagent", "todo" };
            return std::find(
                       std::begin(removed), std::end(removed), tool.spec.name)
                != std::end(removed);
        });
        return tools;
    }

} // namespace

ApplicationState::~ApplicationState()
{
    alive.store(false);
    queue.abandon();
    provider_subscription.disconnect();
    env_subscription.disconnect();
    subagents->stop();
    runner->stop();
}

std::shared_ptr<ApplicationState> make_application_state(
    PostFn post, Config config, StreamFn stream_fn, RuntimeFlag runtime_flags)
{
    std::shared_ptr<ApplicationState> state(new ApplicationState());
    return initialize_root(std::move(state), std::move(post), std::move(config),
        std::move(stream_fn), { }, runtime_flags, true);
}

std::shared_ptr<ApplicationState> make_application_state_with_tools(PostFn post,
    Config config, std::vector<Tool> tools, StreamFn stream_fn,
    RuntimeFlag runtime_flags)
{
    std::shared_ptr<ApplicationState> state(new ApplicationState());
    return initialize_root(std::move(state), std::move(post), std::move(config),
        std::move(stream_fn), std::move(tools), runtime_flags, false);
}

std::shared_ptr<ApplicationState> make_child_application_state(
    const ApplicationState& parent, PostFn post, StreamFn stream_fn,
    ModalRequestFn parent_routing, std::string agent_label)
{
    std::shared_ptr<ApplicationState> state(new ApplicationState());
    std::vector<Tool> tools = default_tools(parent.runtime_flags,
        parent.environment->system()->has_rg, lua_host(state.get()));
    std::erase_if(tools, [](const Tool& tool) {
        return tool.spec.name == "subagent" || tool.spec.name == "todo";
    });
    return initialize_child(std::move(state), parent, std::move(post),
        std::move(stream_fn), std::move(parent_routing), std::move(agent_label),
        std::move(tools));
}

std::shared_ptr<ApplicationState> make_sidechat_application_state(
    ApplicationState& parent)
{
    std::shared_ptr<ApplicationState> state(new ApplicationState());
    state->parent_state = &parent;
    state               = initialize_child(
        std::move(state), parent, parent.post, { },
        [&parent](ModalPayload payload) -> std::future<ModalResult> {
            if (!parent.alive.load()) {
                std::promise<ModalResult> abandoned;
                abandoned.set_value(std::monostate { });
                return abandoned.get_future();
            }
            return request_modal(
                const_cast<ApplicationState&>(parent), std::move(payload));
        },
        "Sidechat", sidechat_roster(parent));
    return state;
}

} // namespace imza
