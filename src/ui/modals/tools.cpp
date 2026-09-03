#include "agent/flows.h"
#include "subsystems/environment.h"
#include "network/json_io.h"
#include "ui/tool_format.h"
#include "ui/ui.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <deque>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "common/util.h"

namespace ursa {

using namespace ftxui;

namespace {

    using namespace ftxui;

    enum class ToolPhase { DECIDE, REASON };

    std::string shell_name(const SystemEnvironment& sys)
    {
        if (sys.default_shell.empty()) {
            return "sh";
        }
        return std::filesystem::path(sys.default_shell).filename().string();
    }

    std::string tool_action_description(const ToolCallRequest& req)
    {
        if (!req.description.empty()) {
            return req.description;
        }
        if (req.name == "shell") {
            return "Run a shell command";
        }
        if (req.name == "edit") {
            return "Replace text in a file";
        }
        if (req.name == "write") {
            return "Write text to a file";
        }
        if (req.name == "read") {
            return "Read text from a file";
        }
        if (req.name == "list") {
            return "List a directory";
        }
        return "Allow this tool to run";
    }

    std::string preview_text(const std::string& content)
    {
        constexpr std::size_t max_lines      = 8;
        const std::vector<std::string> lines = split_lines(content);
        std::string out;
        const std::size_t shown = std::min(lines.size(), max_lines);
        for (std::size_t i = 0; i < shown; ++i) {
            if (!out.empty()) {
                out += '\n';
            }
            out += lines[i];
        }
        if (lines.size() > shown) {
            out += "\n… " + std::to_string(lines.size() - shown) + " more line"
                + (lines.size() - shown == 1 ? "" : "s");
        }
        return out;
    }

    Element tool_approval_reason(const ToolCallRequest& req)
    {
        const Json::Value args = parse_json(req.args);
        const auto string_arg  = [&args](const char* name) {
            return args.isObject() && args[name].isString()
                ? args[name].asString()
                : std::string { };
        };
        std::string message;
        const char* path_key
            = req.name == "edit" || req.name == "write" ? "file_path" : "path";
        if (req.approval_reason
            == ToolCallRequest::ApprovalReason::OUTSIDE_WORKSPACE) {
            const std::string path = string_arg(path_key);
            message = path.empty() ? "Outside workspace"
                                   : "Outside workspace · " + path;
        } else if (req.name == "shell") {
            message = "May modify files or run external processes";
        } else if (req.name == "edit" || req.name == "write") {
            const std::string path = string_arg("file_path");
            message
                = path.empty() ? "Will modify a file" : "Will modify " + path;
        } else if (req.name == "read") {
            const std::string path = string_arg("path");
            message = path.empty() ? "Will read a file" : "Will read " + path;
        } else if (req.name == "list") {
            const std::string path = string_arg("path");
            message
                = path.empty() ? "Will list a directory" : "Will list " + path;
        } else {
            message = "Permission required";
        }
        return text(message) | color(Color::YellowLight);
    }

    Element tool_request_body(
        const ToolCallRequest& req, const SystemEnvironment& system)
    {
        const Json::Value args = parse_json(req.args);
        const auto string_arg  = [&args](const char* name) {
            return args.isObject() && args[name].isString()
                ? args[name].asString()
                : std::string { };
        };

        if (req.name == "shell") {
            const std::string command = string_arg("command").empty()
                ? req.args
                : string_arg("command");
            long timeout              = 10;
            if (args.isObject() && args["timeout"].isIntegral()) {
                timeout = args["timeout"].asInt64();
            }
            std::error_code ec;
            const std::string cwd = std::filesystem::current_path(ec).string();
            const std::string metadata = (ec ? std::string { } : cwd + " · ")
                + "timeout " + std::to_string(timeout) + "s";
            return vbox({ code_block(preview_text(command), shell_name(system)),
                hint_bar(metadata) });
        }

        if (req.name == "edit") {
            Elements rows {
                section_title("Existing text"),
                code_block(preview_text(string_arg("old_string"))),
                section_title("Replacement"),
                code_block(preview_text(string_arg("new_string"))),
            };
            return vbox(std::move(rows));
        }

        if (req.name == "write") {
            return vbox({
                section_title("Content"),
                code_block(preview_text(string_arg("text"))),
            });
        }

        if (req.name == "read") {
            std::string range;
            if (args["line_begin"].isIntegral()) {
                range = "lines " + std::to_string(args["line_begin"].asInt64());
                if (args["line_end"].isIntegral()) {
                    range += "–" + std::to_string(args["line_end"].asInt64());
                } else {
                    range += " onward";
                }
            }
            return range.empty() ? text("") : text(range) | color(PANEL_FG_DIM);
        }

        if (req.name == "list") {
            return text("");
        }

        if (args.isObject() && !args.empty()) {
            Elements rows;
            for (const std::string& key : args.getMemberNames()) {
                const Json::Value& value = args[key];
                const std::string rendered
                    = value.isString() ? value.asString() : write_json(value);
                rows.push_back(hbox({ text(key) | bold | color(PANEL_FG),
                    text("  "), paragraph(preview_text(rendered)) | xflex }));
            }
            return vbox(std::move(rows));
        }
        return code_block(preview_text(req.args), "json");
    }

    std::vector<std::string> wrapped_lines(
        const std::string& body, std::size_t width)
    {
        std::vector<std::string> out;
        std::string line;
        std::string word;
        auto flush_word = [&] {
            if (word.empty()) {
                return;
            }
            if (line.empty()) {
                line = word;
            } else if (line.size() + 1 + word.size() <= width) {
                line += " ";
                line += word;
            } else {
                out.push_back(std::move(line));
                line = word;
            }
            word.clear();
        };
        for (const char c : body) {
            if (c == '\n') {
                flush_word();
                out.push_back(std::move(line));
                line.clear();
            } else if (c == ' ') {
                flush_word();
            } else {
                word += c;
            }
        }
        flush_word();
        if (!line.empty()) {
            out.push_back(std::move(line));
        }
        return out;
    }

    class ModalView : public ComponentBase {
    public:
        explicit ModalView(std::shared_ptr<ApplicationState> state)
            : state_(std::move(state))
            , session_(state_->session)
            , providers_(*state_->providers)
        {
        }

        Element OnRender() override
        {
            const Session& st = *session_;
            if (st.modal().index() == 0) {
                built_ = false;
                return text("");
            }
            ensure_built(st);
            return std::visit(
                [&](const auto& payload) -> Element {
                    using T = std::decay_t<decltype(payload)>;
                    if constexpr (std::is_same_v<T, ToolCallRequest>) {
                        return tool_body(payload);
                    } else if constexpr (std::is_same_v<T, QuestionForm>) {
                        return question_body();
                    } else if constexpr (std::is_same_v<T, ViewerModal>) {
                        return viewer_body(payload);
                    } else if constexpr (std::is_same_v<T, ConnectModal>) {
                        return connect_->Render();
                    } else if constexpr (std::is_same_v<T, VariantModal>) {
                        return variant_->Render();
                    } else if constexpr (std::is_same_v<T, SessionsModal>) {
                        return sessions_->Render();
                    } else if constexpr (std::is_same_v<T, SkillsModal>) {
                        return skills_->Render();
                    }
                    return text("");
                },
                st.modal());
        }

        bool OnEvent(Event event) override
        {
            const Session& st = *session_;
            if (st.modal().index() == 0) {
                return false;
            }
            ensure_built(st);
            if (event == Event::Escape) {
                if (std::holds_alternative<ConnectModal>(st.modal())) {
                    if (body_ && body_->OnEvent(event)) {
                        return true;
                    }
                    ursa::close_modal(*state_);
                    return true;
                }
                if (std::holds_alternative<SessionsModal>(st.modal()) && body_
                    && body_->OnEvent(event)) {
                    return true;
                }
                if (std::holds_alternative<ToolCallRequest>(st.modal())
                    && tool_phase_ == ToolPhase::REASON) {
                    _set_tool_phase(ToolPhase::DECIDE);
                    return true;
                }
                ursa::close_modal(*state_);
                return true;
            }
            if (std::holds_alternative<ViewerModal>(st.modal())) {
                if (event == Event::Return) {
                    ursa::close_modal(*state_);
                    return true;
                }
                return scroll_static(event);
            }
            if (body_) {
                return body_->OnEvent(event);
            }
            return true;
        }

    private:
        struct CardState {
            std::vector<std::string> options;
            std::deque<bool> checked;
            int radio      = 0;
            bool text_live = false;
            std::string free_text;
            int ft_cursor = 0;
            Component selector;
            Component input;
        };

        void ensure_built(const Session& st)
        {
            if (built_ && serial_ == st.modal_serial()) {
                return;
            }
            built_  = true;
            serial_ = st.modal_serial();
            std::visit(
                [this](const auto& payload) { build(payload); }, st.modal());
        }

        void build(const ToolCallRequest&)
        {
            tool_phase_ = ToolPhase::DECIDE;
            reason_buf_.clear();
            reason_cursor_ = 0;

            reason_input_ = Input(field_option(&reason_buf_, &reason_cursor_,
                "optional reason", { }, [this] { _confirm_reject(); }));

            auto resolve = [this](ToolDecision d, std::string r) {
                ursa::resolve_modal(*state_, 
                    ModalResult { ToolVerdict { d, std::move(r) } });
            };
            accept_ = action_button(
                "Allow once", [resolve] { resolve(ToolDecision::ACCEPT, ""); });
            accept_always_ = action_button("Always allow",
                [resolve] { resolve(ToolDecision::ACCEPT_ALWAYS, ""); });
            reject_        = action_button(
                "Reject", [this] { _set_tool_phase(ToolPhase::REASON); });
            confirm_reject_
                = action_button("Reject", [this] { _confirm_reject(); });
            back_ = action_button(
                "Back", [this] { _set_tool_phase(ToolPhase::DECIDE); });

            _build_tool_body();
        }

        void _set_tool_phase(ToolPhase next)
        {
            tool_phase_ = next;
            _build_tool_body();
        }

        void _build_tool_body()
        {
            if (tool_phase_ == ToolPhase::DECIDE) {
                body_ = Container::Horizontal(
                    { accept_, accept_always_, reject_ });
            } else {
                body_ = Container::Vertical({ reason_input_,
                    Container::Horizontal({ confirm_reject_, back_ }) });
                reason_input_->TakeFocus();
            }
        }

        void _confirm_reject()
        {
            ursa::resolve_modal(*state_, ModalResult {
                ToolVerdict { ToolDecision::REJECT, reason_buf_ } });
        }

        void build(const QuestionForm& form)
        {
            form_ = form;
            cards_.clear();
            cards_.reserve(form.size());
            focusables_.clear();

            Components children;
            for (const auto& card : form) {
                cards_.emplace_back();
                CardState& cs = cards_.back();
                cs.checked.assign(card.options.size(), false);

                Components rows;
                for (size_t j = 0; j < card.options.size(); ++j) {
                    rows.push_back(
                        make_option_row(cs, card.options[j], j, card.multi));
                }
                if (card.free_text) {
                    const bool multi    = card.multi;
                    cs.input            = Input(field_option(
                        &cs.free_text, &cs.ft_cursor,
                        card.options.empty() ? "type your answer"
                                             : "type your own answer",
                        [&cs] {
                            if (!trim(cs.free_text).empty()) {
                                cs.text_live = true;
                            }
                        },
                        [this] { submit_question(); }));
                    Component input_row = Renderer(cs.input, [&cs, multi] {
                        const bool live
                            = cs.text_live && !trim(cs.free_text).empty();
                        return hbox({ text(choice_marker(multi, live)),
                            text(" "), cs.input->Render() });
                    });
                    rows.push_back(input_row);
                }

                cs.selector = Container::Vertical(std::move(rows));
                children.push_back(cs.selector);
                focusables_.push_back(cs.selector);
            }
            submit_ = action_button("Submit", [this] { submit_question(); });
            children.push_back(submit_);
            focusables_.push_back(submit_);

            body_ = Container::Vertical(std::move(children));
            if (!focusables_.empty()) {
                focusables_.front()->TakeFocus();
            }
        }

        void build(const ConnectModal& modal)
        {
            if (modal.entry == ConnectModal::Entry::SUBAGENTS) {
                connect_ = make_subagents(state_);
            } else {
                connect_ = make_connect(state_);
            }
            body_ = connect_;
        }

        void build(const VariantModal&)
        {
            variant_ = make_variant(state_);
            body_    = variant_;
        }

        void build(const SessionsModal&)
        {
            sessions_ = make_sessions(state_);
            body_     = sessions_;
        }

        void build(const SkillsModal&)
        {
            skills_ = make_skills(state_);
            body_   = skills_;
        }

        void build(const ViewerModal&) { reset_static_scroll(); }

        void build(std::monostate) { }

        Component make_option_row(
            CardState& cs, const std::string& label, size_t idx, bool multi)
        {
            auto toggle = [&cs, idx, multi] {
                if (multi) {
                    cs.checked[idx] = !cs.checked[idx];
                } else {
                    cs.radio     = static_cast<int>(idx);
                    cs.text_live = false;
                }
            };
            ButtonOption bo;
            bo.transform
                = [&cs, idx, multi, label](const EntryState& s) -> Element {
                const bool selected = multi
                    ? cs.checked[idx]
                    : (!cs.text_live && cs.radio == static_cast<int>(idx));
                return hbox({ text(choice_marker(multi, selected)),
                    choice_label(label, selected, s.focused) });
            };
            bo.on_click   = toggle;
            Component row = Button(label, bo.on_click, bo);
            return space_activates(row, toggle);
        }

        void submit_question()
        {
            ModalAnswer answer;
            for (size_t i = 0; i < form_.size() && i < cards_.size(); ++i) {
                const QuestionCard& card = form_[i];
                const CardState& cs      = cards_[i];
                QuestionAnswer qa;
                qa.prompt       = card.prompt;
                const bool live = card.free_text && cs.text_live
                    && !trim(cs.free_text).empty();
                if (live) {
                    qa.free_text = cs.free_text;
                } else {
                    if (card.multi) {
                        for (size_t j = 0; j < card.options.size(); ++j) {
                            if (cs.checked[j]) {
                                qa.selected.push_back(card.options[j]);
                            }
                        }
                    } else if (!card.options.empty()
                        && cs.radio < static_cast<int>(card.options.size())) {
                        qa.selected.push_back(
                            card.options[static_cast<size_t>(cs.radio)]);
                    }
                }
                answer.cards.push_back(std::move(qa));
            }
            ursa::resolve_modal(*state_, ModalResult { std::move(answer) });
        }

        Element header_line(std::string_view title)
        {
            const size_t remaining = state_->queue.size();
            return hbox({
                text(std::string(title)) | bold,
                filler(),
                text(std::to_string(remaining) + " remaining") | dim
                    | color(PANEL_FG_DIM),
            });
        }

        Element tool_body(const ToolCallRequest& req)
        {
            Elements rows { header_line(tool_display_name(req.name)) };
            rows.push_back(
                text(tool_action_description(req)) | color(PANEL_FG_DIM));
            rows.push_back(tool_approval_reason(req));
            rows.push_back(separatorEmpty());
            rows.push_back(
                tool_request_body(req, *state_->environment->system()));
            rows.push_back(separatorEmpty());
            if (tool_phase_ == ToolPhase::REASON) {
                rows.push_back(section_title("Reason for rejecting", PANEL_FG));
                rows.push_back(reason_input_->Render());
                rows.push_back(hbox({
                    confirm_reject_->Render(),
                    text(" "),
                    back_->Render(),
                }));
                rows.push_back(separatorEmpty());
                rows.push_back(hint_bar("Enter confirm rejection · Esc back"));
            } else {
                rows.push_back(hbox({
                    accept_->Render(),
                    text(" "),
                    accept_always_->Render(),
                    text(" "),
                    reject_->Render(),
                }));
                rows.push_back(separatorEmpty());
                rows.push_back(
                    hint_bar("Always allow applies for this session"));
                rows.push_back(hint_bar("Esc reject"));
            }
            return vbox(std::move(rows)) | xflex;
        }

        Element question_body()
        {
            Elements rows { header_line("Question") };
            for (size_t i = 0; i < form_.size(); ++i) {
                rows.push_back(separatorEmpty());
                rows.push_back(text(form_[i].prompt) | bold);
                if (cards_[i].selector) {
                    rows.push_back(cards_[i].selector->Render());
                }
            }
            rows.push_back(separatorEmpty());
            rows.push_back(submit_->Render() | center);
            rows.push_back(separatorEmpty());
            rows.push_back(hint_bar("↑/↓ navigate · Enter confirm"));
            return vbox(std::move(rows)) | xflex;
        }

        Element viewer_body(const ViewerModal& payload)
        {
            Element body;
            if (payload.line_numbers) {
                body = code_block_with_lines(
                    payload.content, payload.lang, payload.start_line);
            } else {
                const int popup_w
                    = std::min(Terminal::Size().dimx - 4, MODAL_MAX_WIDTH);
                const std::size_t content_w
                    = static_cast<std::size_t>(std::max(40, popup_w - 8));
                body = code_block(
                    join_lines(wrapped_lines(payload.content, content_w)),
                    payload.lang);
            }
            return vbox({ header_line(payload.title), separatorEmpty(),
                       static_viewport(std::move(body), payload.metadata) })
                | xflex;
        }

        Element static_viewport(Element content, const std::string& metadata)
        {
            static_view_.scroll_lines(0);
            Element viewport = std::move(content)
                | capture_content_height(&static_view_.content_height)
                | vscroll_indicator
                | focusPosition(0,
                    static_view_.scroll
                        + std::max(0, static_view_.viewport_lines() - 1) / 2)
                | yframe | yflex | reflect(static_view_.box);
            Elements rows { std::move(viewport), separatorEmpty() };
            if (!metadata.empty()) {
                rows.push_back(hint_bar(metadata));
            }
            rows.push_back(hint_bar("↑/↓ scroll · Esc close"));
            return vbox(std::move(rows)) | xflex | yflex;
        }

        bool scroll_static(Event event)
        {
            if (event == Event::ArrowUp) {
                static_view_.scroll_lines(-1);
                return true;
            }
            if (event == Event::ArrowDown) {
                static_view_.scroll_lines(1);
                return true;
            }
            if (event == Event::PageUp) {
                static_view_.scroll_lines(
                    -std::max(1, static_view_.viewport_lines() - 1));
                return true;
            }
            if (event == Event::PageDown) {
                static_view_.scroll_lines(
                    std::max(1, static_view_.viewport_lines() - 1));
                return true;
            }
            if (event == Event::Home) {
                static_view_.scroll = 0;
                return true;
            }
            if (event == Event::End) {
                static_view_.scroll = static_view_.max_scroll();
                return true;
            }
            if (event.is_mouse()) {
                const Mouse& m = event.mouse();
                if (m.button == Mouse::WheelUp) {
                    static_view_.scroll_lines(-3);
                    return true;
                }
                if (m.button == Mouse::WheelDown) {
                    static_view_.scroll_lines(3);
                    return true;
                }
            }
            return false;
        }

        void reset_static_scroll() { static_view_ = { }; }

        std::shared_ptr<ApplicationState> state_;
        std::shared_ptr<Session> session_;
        ProviderStore& providers_;
        bool built_           = false;
        std::uint64_t serial_ = 0;

        QuestionForm form_;
        std::vector<CardState> cards_;
        std::vector<Component> focusables_;
        Component submit_;

        ToolPhase tool_phase_ = ToolPhase::DECIDE;
        std::string reason_buf_;
        int reason_cursor_ = 0;
        ScrollView static_view_ { };
        Component reason_input_;
        Component accept_;
        Component accept_always_;
        Component reject_;
        Component confirm_reject_;
        Component back_;
        Component connect_;
        Component variant_;
        Component sessions_;
        Component skills_;

        Component body_;
    };

} // namespace

ftxui::Component make_modal(std::shared_ptr<ApplicationState> state)
{
    return ftxui::Make<ModalView>(std::move(state));
}

} // namespace ursa
