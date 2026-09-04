#include "agent/application_state.h"
#include "agent/flows.h"
#include "agent/turn_runner.h"
#include "subsystems/delegation_runner.h"
#include "subsystems/review.h"
#include "subsystems/skill_store.h"

#include <utility>

namespace ursa {

namespace {

    PostFn guarded_post(ApplicationState* state, PostFn post)
    {
        return [state, post = std::move(post)](std::function<void()> f) {
            if (state->alive.load()) {
                post(std::move(f));
            }
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
        state->delegation = std::make_unique<DelegationRunner>(
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

ApplicationState::~ApplicationState()
{
    alive.store(false);
    queue.abandon();
    provider_subscription.disconnect();
    env_subscription.disconnect();
    subagents->stop();
    runner->stop();
}

std::shared_ptr<ApplicationState> make_application_state(PostFn post,
    Config config, StreamFn stream_fn, std::vector<Tool> tools,
    ModalRequestFn parent_routing, std::string agent_label)
{
    std::shared_ptr<ApplicationState> state(new ApplicationState());
    state->session        = std::make_shared<Session>();
    state->providers      = std::make_shared<ProviderStore>(std::move(config));
    state->subagents      = std::make_shared<SubagentManager>();
    state->environment    = std::make_shared<Environment>();
    state->review         = std::make_shared<ReviewState>();
    state->skills         = std::make_shared<SkillStore>();
    state->post           = guarded_post(state.get(), std::move(post));
    state->on_exit        = [] { };
    state->parent_routing = std::move(parent_routing);
    state->agent_label    = std::move(agent_label);
    wire(state, std::move(stream_fn), std::move(tools));
    return state;
}

std::shared_ptr<ApplicationState> make_child_application_state(
    const ApplicationState& parent, PostFn post, StreamFn stream_fn,
    std::vector<Tool> tools, ModalRequestFn parent_routing,
    std::string agent_label)
{
    std::shared_ptr<ApplicationState> state(new ApplicationState());
    state->session        = std::make_shared<Session>();
    state->providers      = parent.providers;
    state->subagents      = std::make_shared<SubagentManager>();
    state->environment    = parent.environment;
    state->review         = std::make_shared<ReviewState>();
    state->skills         = std::make_shared<SkillStore>();
    state->post           = guarded_post(state.get(), std::move(post));
    state->on_exit        = [] { };
    state->parent_routing = std::move(parent_routing);
    state->agent_label    = std::move(agent_label);
    wire(state, std::move(stream_fn), std::move(tools));
    return state;
}

} // namespace ursa
