#include "agent/flows.h"
#include "common/modal.h"
#include "ui/ui.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <vector>

namespace ursa {

using namespace ftxui;

namespace {

    class VariantView : public ComponentBase {
    public:
        VariantView(std::shared_ptr<ApplicationState> state)
            : state_(std::move(state))
            , session_(state_->session)
        {
            const auto modal = std::get<VariantModal>(session_->modal());
            options_         = modal.options;
            const auto found
                = std::find(options_.begin(), options_.end(), modal.current);
            if (found != options_.end()) {
                current_ = static_cast<int>(found - options_.begin());
            }
            cursor_ = current_;
        }

        Element OnRender() override
        {
            Elements rows;
            rows.push_back(text("Reasoning effort") | bold);
            rows.push_back(separatorEmpty());
            for (int i = 0; i < static_cast<int>(options_.size()); ++i) {
                const std::string option
                    = options_[static_cast<std::size_t>(i)];
                rows.push_back(hbox({
                    text(choice_marker(false, i == current_)),
                    choice_label(option, i == current_, i == cursor_),
                }));
            }
            rows.push_back(separatorEmpty());
            rows.push_back(text("arrows navigate · Enter select · Esc close")
                | dim | center);
            return vbox({ vbox(std::move(rows)), separatorEmpty() }) | xflex;
        }

        bool OnEvent(Event event) override
        {
            if (event == Event::Escape) {
                ursa::close_modal(*state_);
                return true;
            }
            if (event == Event::Return) {
                apply();
                return true;
            }
            return move_list_cursor(
                event, cursor_, static_cast<int>(options_.size()));
        }

    private:
        void apply()
        {
            if (cursor_ >= 0 && cursor_ < static_cast<int>(options_.size())) {
                ursa::resolve_modal(*state_,
                    ModalResult { VariantChoice { options_[cursor_] } });
            }
        }

        std::shared_ptr<ApplicationState> state_;
        std::shared_ptr<Session> session_;
        std::vector<std::string> options_;
        int current_ = 0;
        int cursor_  = 0;
    };

} // namespace

ftxui::Component make_variant(std::shared_ptr<ApplicationState> state)
{
    return ftxui::Make<VariantView>(state);
}

} // namespace ursa
