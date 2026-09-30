#include "app/flows.h"
#include "common/modal.h"
#include "ui/ui.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <vector>

namespace imza {

using namespace ftxui;

namespace {

    class VariantView : public ComponentBase {
    public:
        VariantView(std::shared_ptr<ApplicationState> state)
            : _state(std::move(state))
        {
            const auto modal = std::get<VariantModal>(_state->session->modal());
            _options         = modal.options;
            const auto found
                = std::find(_options.begin(), _options.end(), modal.current);
            if (found != _options.end()) {
                _current = static_cast<int>(found - _options.begin());
            }
            _cursor = _current;
        }

        Element OnRender() override
        {
            Elements rows = modal_header("Reasoning effort");
            for (int i = 0; i < static_cast<int>(_options.size()); ++i) {
                const std::string option
                    = _options[static_cast<std::size_t>(i)];
                rows.push_back(hbox({
                    text(choice_marker(false, i == _current)),
                    choice_label(option, i == _current, i == _cursor),
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
                imza::close_modal(*_state);
                return true;
            }
            if (event == Event::Return) {
                _apply();
                return true;
            }
            return move_list_cursor(
                event, _cursor, static_cast<int>(_options.size()));
        }

    private:
        void _apply()
        {
            if (_cursor >= 0 && _cursor < static_cast<int>(_options.size())) {
                imza::resolve_modal(*_state,
                    ModalResult { VariantChoice { _options[_cursor] } });
            }
        }

        std::shared_ptr<ApplicationState> _state;
        std::vector<std::string> _options;
        int _current = 0;
        int _cursor  = 0;
    };

} // namespace

ftxui::Component make_variant(std::shared_ptr<ApplicationState> state)
{
    return ftxui::Make<VariantView>(state);
}

} // namespace imza
