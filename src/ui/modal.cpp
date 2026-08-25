#include "ui.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "util.h"

namespace ursa {

using namespace ftxui;

namespace {

    using namespace ftxui;

    enum class ToolPhase { DECIDE, REASON };

    InputOption field_option(std::string* content, int* cursor,
        std::string placeholder, std::function<void()> on_enter,
        std::function<void()> on_change = {})
    {
        InputOption io;
        io.content         = content;
        io.cursor_position = cursor;
        io.placeholder     = std::move(placeholder);
        io.multiline       = false;
        io.on_enter        = std::move(on_enter);
        io.on_change       = std::move(on_change);
        io.transform       = [](InputState state) {
            if (state.is_placeholder) {
                state.element |= dim;
            }
            state.element |= bgcolor(
                state.focused ? PANEL_COLOR_FOCUS : PANEL_COLOR);
            return state.element;
        };
        return io;
    }

    class ModalImpl : public ComponentBase {
    public:
        explicit ModalImpl(Controller& controller)
            : controller_(controller)
        {
        }

        Element OnRender() override
        {
            const UiState& st = controller_.state();
            if (st.modal.index() == 0) {
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
                    } else if constexpr (std::is_same_v<T, SettingsModal>) {
                        return settings_body(payload);
                    } else if constexpr (std::is_same_v<T, HelpModal>) {
                        return help_body();
                    } else if constexpr (std::is_same_v<T, SystemPromptModal>) {
                        return system_prompt_body(payload);
                    }
                    return text("");
                },
                st.modal);
        }

        bool OnEvent(Event event) override
        {
            const UiState& st = controller_.state();
            if (st.modal.index() == 0) {
                return false;
            }
            ensure_built(st);
            if (event == Event::Escape) {
                if (std::holds_alternative<ToolCallRequest>(st.modal)
                    && tool_phase_ == ToolPhase::REASON) {
                    _set_tool_phase(ToolPhase::DECIDE);
                    return true;
                }
                controller_.close_modal();
                return true;
            }
            if (std::holds_alternative<QuestionForm>(st.modal)
                && (event == Event::Tab || event == Event::TabReverse)) {
                _cycle_focus(event == Event::Tab);
                return true;
            } else if (std::holds_alternative<HelpModal>(st.modal)) {
                if (event == Event::Return) {
                    controller_.close_modal();
                    return true;
                }
                return true;
            } else if (std::holds_alternative<SystemPromptModal>(st.modal)) {
                if (event == Event::Return) {
                    controller_.close_modal();
                    return true;
                }
                return true;
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
            int radio = 0;
            bool text_live = false;
            std::string free_text;
            int ft_cursor = 0;
            Component selector;
            Component input;
        };

        void ensure_built(const UiState& st)
        {
            if (built_ && serial_ == st.modal_serial) {
                return;
            }
            built_  = true;
            serial_ = st.modal_serial;
            std::visit(
                [this](const auto& payload) { build(payload); }, st.modal);
        }

        void build(const ToolCallRequest&)
        {
            kind_        = Kind::TOOL;
            tool_phase_  = ToolPhase::DECIDE;
            reason_buf_.clear();
            reason_cursor_ = 0;

            reason_input_ = Input(field_option(
                &reason_buf_, &reason_cursor_, "Reason",
                [this] { _confirm_reject(); }));

            auto resolve = [this](ToolDecision d, std::string r) {
                controller_.resolve_modal(
                    ModalResult { ToolVerdict { d, std::move(r) } });
            };
            accept_ = with_space(
                Button("Accept", [resolve] { resolve(ToolDecision::ACCEPT, ""); }),
                [resolve] { resolve(ToolDecision::ACCEPT, ""); });
            accept_always_ = with_space(
                Button("Accept Always", [resolve] {
                    resolve(ToolDecision::ACCEPT_ALWAYS, "");
                }),
                [resolve] { resolve(ToolDecision::ACCEPT_ALWAYS, ""); });
            reject_ = with_space(
                Button("Reject", [this] { _set_tool_phase(ToolPhase::REASON); }),
                [this] { _set_tool_phase(ToolPhase::REASON); });
            confirm_reject_ = with_space(
                Button("Reject", [this] { _confirm_reject(); }),
                [this] { _confirm_reject(); });
            back_ = with_space(
                Button("Back", [this] { _set_tool_phase(ToolPhase::DECIDE); }),
                [this] { _set_tool_phase(ToolPhase::DECIDE); });

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
                body_ = Container::Vertical(
                    { reason_input_,
                        Container::Horizontal({ confirm_reject_, back_ }) });
                reason_input_->TakeFocus();
            }
        }

        void _confirm_reject()
        {
            controller_.resolve_modal(ModalResult {
                ToolVerdict { ToolDecision::REJECT, reason_buf_ } });
        }

        void build(const QuestionForm& form)
        {
            kind_ = Kind::QUESTION;
            form_ = form;
            cards_.clear();
            cards_.reserve(form.size());
            focusables_.clear();
            focus_index_ = 0;

            Components children;
            for (const auto& card : form) {
                cards_.emplace_back();
                CardState& cs = cards_.back();
                cs.options    = card.options;
                cs.checked.assign(card.options.size(), false);

                Components rows;
                for (size_t j = 0; j < card.options.size(); ++j) {
                    rows.push_back(
                        make_option_row(cs, card.options[j], j, card.multi));
                }
                if (card.free_text) {
                    const bool multi = card.multi;
                    cs.input = Input(field_option(&cs.free_text, &cs.ft_cursor,
                        card.options.empty()
                            ? "type your answer"
                            : "type your own answer",
                        [this] { submit_question(); },
                        [this, &cs] {
                            if (!trim(cs.free_text).empty()) {
                                cs.text_live = true;
                            }
                        }));
                    Component input_row = Renderer(cs.input, [this, &cs, multi] {
                        const bool live
                            = cs.text_live && !trim(cs.free_text).empty();
                        const std::string marker
                            = multi ? (live ? "▣ " : "☐ ")
                                    : (live ? "◉ " : "○ ");
                        return hbox(
                            { text(marker), text(" "), cs.input->Render() });
                    });
                    rows.push_back(input_row);
                }

                cs.selector = Container::Vertical(std::move(rows));
                children.push_back(cs.selector);
                focusables_.push_back(cs.selector);
            }
            submit_ = with_space(
                Button("Submit", [this] { submit_question(); }),
                [this] { submit_question(); });
            children.push_back(submit_);
            focusables_.push_back(submit_);

            body_ = Container::Vertical(std::move(children));
            if (!focusables_.empty()) {
                focus_index_ = 0;
                focusables_.front()->TakeFocus();
            }
        }

        void build(const SettingsModal&)
        {
            kind_     = Kind::SETTINGS;
            settings_ = make_settings(controller_);
            body_     = settings_;
        }

        void build(const HelpModal&) { kind_ = Kind::HELP; }

        void build(const SystemPromptModal&) { kind_ = Kind::SYSTEM_PROMPT; }

        void build(std::monostate) { kind_ = Kind::NONE; }

        void _cycle_focus(bool forward)
        {
            if (focusables_.empty()) {
                return;
            }
            const int n = static_cast<int>(focusables_.size());
            focus_index_ = (focus_index_ + (forward ? 1 : -1) + n) % n;
            focusables_[focus_index_]->TakeFocus();
        }

        Component make_option_row(CardState& cs, const std::string& label,
            size_t idx, bool multi)
        {
            auto toggle = [&cs, idx, multi] {
                if (multi) {
                    cs.checked[idx] = !cs.checked[idx];
                } else {
                    cs.radio      = static_cast<int>(idx);
                    cs.text_live = false;
                }
            };
            ButtonOption bo;
            bo.transform = [&cs, idx, multi, &label](const EntryState& s) -> Element {
                const bool selected
                    = multi
                        ? cs.checked[idx]
                        : (!cs.text_live
                            && cs.radio == static_cast<int>(idx));
                const std::string marker
                    = multi ? (selected ? "▣ " : "☐ ")
                            : (selected ? "◉ " : "○ ");
                Element e = text(label);
                if (s.focused) {
                    e = std::move(e) | bold | color(PANEL_FG) | inverted;
                } else if (selected) {
                    e = std::move(e) | bold | color(PANEL_FG);
                } else {
                    e = std::move(e) | color(PANEL_FG_DIM);
                }
                return hbox({ text(marker), std::move(e) });
            };
            bo.on_click = toggle;
            Component row = Button(label, bo.on_click, bo);
            return CatchEvent(row, [toggle](Event e) -> bool {
                if (e == Event::Character(' ')) {
                    toggle();
                    return true;
                }
                return false;
            });
        }

        Component with_space(Component child, std::function<void()> on_space)
        {
            return CatchEvent(child, [on_space](Event e) -> bool {
                if (e == Event::Character(' ')) {
                    on_space();
                    return true;
                }
                return false;
            });
        }

        void submit_question()
        {
            ModalAnswer answer;
            for (size_t i = 0; i < form_.size() && i < cards_.size(); ++i) {
                const QuestionCard& card = form_[i];
                const CardState& cs      = cards_[i];
                QuestionAnswer qa;
                qa.prompt = card.prompt;
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
                        && cs.radio
                            < static_cast<int>(card.options.size())) {
                        qa.selected.push_back(
                            card.options[static_cast<size_t>(cs.radio)]);
                    }
                }
                answer.cards.push_back(std::move(qa));
            }
            controller_.resolve_modal(ModalResult { std::move(answer) });
        }

        Element header_line(std::string_view title)
        {
            const size_t remaining = controller_.queue_size();
            return hbox({
                text(std::string(title)) | bold,
                filler(),
                text(std::to_string(remaining) + " remaining") | dim
                    | color(PANEL_FG_DIM),
            });
        }

        Element tool_body(const ToolCallRequest& req)
        {
            Elements rows { header_line("Tool call") };
            rows.push_back(separatorEmpty());
            if (req.name == "shell") {
                const Json::Value parsed = parse_json(req.args);
                std::string cmd = req.args;
                if (parsed.isObject() && parsed["command"].isString()) {
                    cmd = parsed["command"].asString();
                }
                rows.push_back(code_block(cmd, controller_.shell_name()));
            } else {
                rows.push_back(code_block(req.args));
            }
            rows.push_back(separatorEmpty());
            if (tool_phase_ == ToolPhase::REASON) {
                rows.push_back(reason_input_->Render());
                rows.push_back(hbox({
                    confirm_reject_->Render(),
                    text(" "),
                    back_->Render(),
                }));
                rows.push_back(separatorEmpty());
                rows.push_back(
                    text("Enter confirms rejection · Esc goes back") | dim
                    | color(PANEL_FG_DIM));
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
                    text("Esc cancels") | dim | color(PANEL_FG_DIM));
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
            rows.push_back(
                text("Arrows move between options · Enter confirms") | dim
                | color(PANEL_FG_DIM));
            return vbox(std::move(rows)) | xflex;
        }

        Element settings_body(const SettingsModal&)
        {
            return settings_->Render();
        }

        Element help_body() { return render_help(controller_.commands()); }

        Element system_prompt_body(const SystemPromptModal& payload)
        {
            Elements rows { header_line("System prompt") };
            rows.push_back(separatorEmpty());
            rows.push_back(code_block(payload.prompt, "text"));
            rows.push_back(separatorEmpty());
            rows.push_back(
                text("Esc closes") | dim | color(PANEL_FG_DIM));
            return vbox(std::move(rows)) | xflex;
        }

        enum class Kind { NONE, TOOL, QUESTION, SETTINGS, HELP, SYSTEM_PROMPT };

        Controller& controller_;
        Kind kind_            = Kind::NONE;
        bool built_           = false;
        std::uint64_t serial_ = 0;

        QuestionForm form_;
        std::vector<CardState> cards_;
        std::vector<Component> focusables_;
        int focus_index_ = 0;
        Component submit_;

        ToolPhase tool_phase_   = ToolPhase::DECIDE;
        std::string reason_buf_;
        int reason_cursor_ = 0;
        Component reason_input_;
        Component accept_;
        Component accept_always_;
        Component reject_;
        Component confirm_reject_;
        Component back_;
        Component settings_;

        Component body_;
    };

} // namespace

ftxui::Component make_modal(Controller& controller)
{
    return ftxui::Make<ModalImpl>(controller);
}

ftxui::Element render_help(const std::vector<SlashCommand>& commands)
{
    Elements rows;
    for (const auto& c : commands) {
        rows.push_back(hbox({
            text(c.name) | bold | color(PANEL_FG),
            text("   "),
            text(c.desc) | dim | color(PANEL_FG_DIM),
        }));
    }
    return vbox({
        section_title("Commands"),
        vbox(std::move(rows)) | borderStyled(ROUNDED, PANEL_BORDER),
    });
}

} // namespace ursa
