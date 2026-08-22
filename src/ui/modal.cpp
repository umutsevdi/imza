#include "ui.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "render.h"

namespace ursa {

namespace {

    using namespace ftxui;

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
                controller_.close_modal();
                return true;
            }
            if (std::holds_alternative<QuestionForm>(st.modal)) {
                if (event == Event::Return) {
                    submit_question();
                    return true;
                }
            } else if (std::holds_alternative<HelpModal>(st.modal)) {
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
            kind_ = Kind::TOOL;
            reason_buf_.clear();
            reason_cursor_ = 0;

            InputOption io;
            io.content         = &reason_buf_;
            io.cursor_position = Ref<int>(&reason_cursor_);
            io.placeholder     = "why?";
            io.multiline       = false;
            io.transform       = [](InputState state) {
                if (state.is_placeholder) {
                    state.element |= dim;
                }
                state.element |= bgcolor(PANEL_COLOR);
                return state.element;
            };
            reason_input_ = Input(io);

            auto resolve = [this](ToolVerdict verdict) {
                controller_.resolve_modal(ModalResult { std::move(verdict) });
            };
            accept_ = Button(
                "Accept", [resolve] { resolve({ ToolDecision::ACCEPT, "" }); });
            accept_always_ = Button("Accept Always",
                [resolve] { resolve({ ToolDecision::ACCEPT_ALWAYS, "" }); });
            reject_        = Button("Reject", [this, resolve] {
                resolve({ ToolDecision::REJECT, reason_buf_ });
            });

            body_ = Container::Vertical({
                reason_input_,
                Container::Horizontal({ accept_, accept_always_, reject_ }),
            });
        }

        void build(const QuestionForm& form)
        {
            kind_ = Kind::QUESTION;
            form_ = form;
            cards_.clear();
            cards_.reserve(form.size());

            for (const auto& card : form) {
                cards_.emplace_back();
                CardState& cs = cards_.back();
                cs.options    = card.options;
                cs.checked.assign(card.options.size(), false);
            }

            Components children;
            for (size_t i = 0; i < form.size(); ++i) {
                const QuestionCard& card = form[i];
                CardState& cs            = cards_[i];
                if (card.multi) {
                    Components checks;
                    for (size_t j = 0; j < card.options.size(); ++j) {
                        checks.push_back(
                            Checkbox(card.options[j], &cs.checked[j]));
                    }
                    cs.selector = Container::Vertical(std::move(checks));
                } else if (!card.options.empty()) {
                    cs.selector = Radiobox(&cs.options, &cs.radio);
                }
                if (card.free_text) {
                    InputOption io;
                    io.content         = &cs.free_text;
                    io.cursor_position = Ref<int>(&cs.ft_cursor);
                    io.placeholder     = "type your answer";
                    io.multiline       = false;
                    io.transform       = [](InputState state) {
                        if (state.is_placeholder) {
                            state.element |= dim;
                        }
                        state.element |= bgcolor(PANEL_COLOR);
                        return state.element;
                    };
                    cs.input = Input(io);
                }
                if (cs.selector) {
                    children.push_back(cs.selector);
                }
                if (cs.input) {
                    children.push_back(cs.input);
                }
            }
            submit_ = Button("Submit", [this] { submit_question(); });
            children.push_back(submit_);

            body_ = Container::Vertical(std::move(children));
        }

        void build(const SettingsModal&)
        {
            kind_     = Kind::SETTINGS;
            settings_ = make_settings(controller_);
            body_     = settings_;
        }

        void build(const HelpModal&) { kind_ = Kind::HELP; }

        void build(std::monostate) { kind_ = Kind::NONE; }

        void submit_question()
        {
            ModalAnswer answer;
            for (size_t i = 0; i < form_.size() && i < cards_.size(); ++i) {
                const QuestionCard& card = form_[i];
                const CardState& cs      = cards_[i];
                QuestionAnswer qa;
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
                if (card.free_text) {
                    qa.free_text = cs.free_text;
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
            Elements code_lines;
            for (size_t pos = 0; pos < req.args.size();) {
                const size_t nl        = req.args.find('\n', pos);
                const std::string line = req.args.substr(
                    pos, nl == std::string::npos ? nl : nl - pos);
                code_lines.push_back(text(line));
                if (nl == std::string::npos) {
                    break;
                }
                pos = nl + 1;
            }
            Element args_block
                = vbox(std::move(code_lines)) | bgcolor(PANEL_COLOR);

            Elements rows { header_line("Tool call") };
            if (!req.description.empty()) {
                rows.push_back(text(req.description) | dim);
            }
            rows.push_back(args_block);
            rows.push_back(separatorEmpty());
            rows.push_back(reason_input_->Render());
            rows.push_back(hbox({
                accept_->Render(),
                text(" "),
                accept_always_->Render(),
                text(" "),
                reject_->Render(),
            }));
            rows.push_back(separatorEmpty());
            rows.push_back(
                text("Esc cancels · typed text becomes the rejection reason")
                | dim | color(PANEL_FG_DIM));
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
                if (cards_[i].input) {
                    rows.push_back(cards_[i].input->Render());
                }
            }
            rows.push_back(separatorEmpty());
            rows.push_back(submit_->Render() | center);
            return vbox(std::move(rows)) | xflex;
        }

        Element settings_body(const SettingsModal&)
        {
            return settings_->Render();
        }

        Element help_body() { return render_help(controller_.commands()); }

        enum class Kind { NONE, TOOL, QUESTION, SETTINGS, HELP };

        Controller& controller_;
        Kind kind_            = Kind::NONE;
        bool built_           = false;
        std::uint64_t serial_ = 0;

        QuestionForm form_;
        std::vector<CardState> cards_;
        Component submit_;

        std::string reason_buf_;
        int reason_cursor_ = 0;
        Component reason_input_;
        Component accept_;
        Component accept_always_;
        Component reject_;
        Component settings_;

        Component body_;
    };

} // namespace

ftxui::Component make_modal(Controller& controller)
{
    return ftxui::Make<ModalImpl>(controller);
}

} // namespace ursa
