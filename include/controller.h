#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "application_state.h"
#include "environment.h"
#include "network.h"
#include "provider_store.h"
#include "session.h"
#include "slash_commands.h"
#include "subagent_manager.h"
#include "tools.h"
#include "types.h"

namespace ursa {

struct LayoutCtx {
    enum class Kind { WIDE, NARROW };
    static constexpr int wide_threshold = 100;
    static constexpr int panel_width    = 40;
    Kind kind;
    int width;
};

using PostFn = std::function<void(std::function<void()>)>;
using StreamFn
    = std::function<Status(const ChatRequest&, const StreamCallback&)>;
struct TurnSettings {
    std::string model;
    std::string reasoning_effort;
    Session::Mode mode  = Session::Mode::PLAN;
    ApiStandard dialect = ApiStandard::OPENAI;
    std::string connection_id;
    Route route;
};

class Controller {
public:
    Controller(std::shared_ptr<ApplicationState> state, PostFn post,
        std::function<void()> on_exit, StreamFn stream_fn = { },
        std::vector<Tool> tools = { });
    Controller(std::shared_ptr<Session> session, const Config& cfg, PostFn post,
        std::function<void()> on_exit, StreamFn stream_fn = { },
        std::vector<Tool> tools = { }, ModelsFn models_fn = { });
    Controller(std::shared_ptr<Session> session,
        std::shared_ptr<ProviderStore> providers, PostFn post,
        std::function<void()> on_exit, StreamFn stream_fn = { },
        std::vector<Tool> tools = { });
    ~Controller();

    Controller(const Controller&)            = delete;
    Controller& operator=(const Controller&) = delete;

    void submit(std::string text,
        std::vector<FileAttachment> attachments = { });
    void set_mode(Session::Mode next_mode);
    void set_error(std::string msg);
    void clear_error();
    void close_modal();
    void resolve_modal(ModalResult result);
    void enqueue_user_modal(ModalPayload payload);
    void cancel_queued(std::size_t id);
    void delete_saved_session(const std::filesystem::path& path);
    void interrupt();
    size_t queue_size() const;
    std::pair<SkillCounts, SkillCounts> skill_counts() const;
    std::vector<Skill> available_skills() const;
    SubagentHandle run_subagent(std::string prompt, std::string model,
        std::string variant, bool visible);
    std::span<const SlashCommand> commands() const { return slash_commands(); }

private:
    void submit_message(
        std::string text, std::vector<FileAttachment> attachments);
    void _submit_with_skills(
        std::string text, std::vector<FileAttachment> attachments);
    void run_slash(std::string_view cmd);
    void finish(std::string error);

    void _post(std::function<void()> f);
    std::string _system_prompt() const;
    void _spawn(
        std::vector<Message> history, StreamFn override, TurnSettings settings);
    void _spawn_title(std::string input, TurnSettings settings);
    void _drive(
        std::vector<Message> history, StreamFn override, TurnSettings settings);
    bool _compact_history(std::vector<Message>& history, StreamFn override,
        const TurnSettings& settings, std::uint64_t prompt_tokens);
    void _begin_connect(const ConnectResult& res);
    void _new_session();
    void _apply_pick(const ModelChoice& choice);
    SessionsModal _sessions_modal() const;
    SkillsModal _skills_modal() const;
    std::optional<Skill> _resolve_skill(const Json::Value& args) const;
    SkillPolicy _skill_policy(const Skill& skill) const;
    bool _validate_skill_mentions(std::string_view text);
    std::vector<Skill> _mentioned_skills(std::string_view text) const;
    bool _load_skill(const Skill& skill);

    void _drain_pending_asks(std::vector<Message>& history,
        std::string& reply_buffer, const std::string& assistant_text,
        ApiStandard dialect);
    void _apply_tool_result(const ToolCallRequest& req, const ModalResult& res,
        std::vector<Message>& tool_msgs);
    bool _model_reasons(const std::string& model) const;
    std::uint64_t _budget_for_effort(const std::string& effort) const;
    void _set_reasoning(
        ChatRequest& req, ApiStandard dialect, std::string_view effort);
    void _apply_question_result(
        const ModalResult& res, std::string& reply_buffer);
    void _apply_ask_result(const ToolCallRequest& req, const ModalResult& res,
        std::vector<Message>& tool_msgs);
    void _run_tool(const ToolCallRequest& req, std::vector<Message>& tool_msgs);
    void _present_front();
    void _drain_queued();

    std::shared_ptr<ApplicationState> state_;
    PostFn post_;
    std::function<void()> on_exit_;
    StreamFn stream_fn_;
    bool has_stream_override_ { false };
    std::vector<Tool> tools_;
    std::vector<ToolSpec> specs_plan_;
    std::vector<ToolSpec> specs_all_;
    Signal<>::Subscription env_sub_;
    Signal<>::Subscription provider_sub_;

    struct PendingModal {
        ModalPayload payload;
        std::shared_ptr<std::promise<ModalResult>> promise;
    };
    std::deque<PendingModal> queue_;
    mutable std::mutex queue_mutex_;
    std::set<std::string> allowed_tools_;
    std::set<std::string> loaded_skills_;
    std::map<std::string, std::string> loaded_skill_contents_;
    mutable std::mutex loaded_skills_mutex_;

    struct PendingSkillTurn {
        std::string text;
        std::vector<FileAttachment> attachments;
        std::vector<Skill> awaiting;
        std::size_t next = 0;
    };
    std::optional<PendingSkillTurn> pending_skill_turn_;

    std::vector<StreamEvent> stream_events_;
    std::atomic<bool> alive_ { true };
    std::optional<std::jthread> worker_;
    int retry_after_secs_ = 0;
};

std::string error_text(Status st);

} // namespace ursa
