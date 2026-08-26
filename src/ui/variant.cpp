#include "ui.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <string>
#include <vector>

namespace ursa {

namespace {

    using namespace ftxui;

    class VariantImpl : public ComponentBase {
    public:
        VariantImpl(Controller& controller)
            : controller_(controller)
        {
            const auto& modal = std::get<VariantModal>(controller_.state().modal);
            options_          = modal.options;
            selected_         = 0;
            for (size_t i = 0; i < options_.size(); ++i) {
                if (options_[i] == modal.current) {
                    selected_ = static_cast<int>(i);
                    break;
                }
            }
            radiobox_ = Radiobox(&options_, &selected_);
            apply_    = action_button("Apply (Enter)", [&] { apply(); });
            container_ = Container::Vertical({ radiobox_, apply_ });
            Add(container_);
        }

        Element OnRender() override
        {
            Elements rows;
            rows.push_back(
                section_title("Reasoning effort", Color::GrayLight));
            rows.push_back(radiobox_->Render());
            rows.push_back(separatorEmpty());
            rows.push_back(apply_->Render() | center);
            rows.push_back(separatorEmpty());
            rows.push_back(text("arrows navigate · Enter apply · Esc close")
                | dim | center);
            return vbox({ separatorEmpty(), vbox(std::move(rows)),
                separatorEmpty() })
                | xflex;
        }

        bool OnEvent(Event event) override
        {
            if (event == Event::Escape) {
                controller_.close_modal();
                return true;
            }
            if (event == Event::Return) {
                apply();
                return true;
            }
            return container_->OnEvent(event);
        }

    private:
        void apply()
        {
            if (selected_ >= 0
                && selected_ < static_cast<int>(options_.size())) {
                controller_.resolve_modal(
                    ModalResult { VariantChoice { options_[selected_] } });
            }
        }

        Controller& controller_;
        std::vector<std::string> options_;
        int selected_ = 0;
        Component radiobox_;
        Component apply_;
        Component container_;
    };

} // namespace

ftxui::Component make_variant(Controller& controller)
{
    return ftxui::Make<VariantImpl>(controller);
}

} // namespace ursa
