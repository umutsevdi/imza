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

    class SettingsImpl : public ComponentBase {
    public:
        SettingsImpl(Controller& controller)
            : controller_(controller)
        {
            capabilities_ = {
                { "List files", true },
                { "Search code", true },
                { "Read files", true },
                { "Write code", true },
                { "Run shell", false },
                { "Web fetch", false },
            };
            themes_ = { "Dark", "Darker", "Midnight" };

            Components checks;
            checks.reserve(capabilities_.size());
            for (auto& [label, on] : capabilities_) {
                checks.push_back(Checkbox(label, &on));
            }

            radiobox_ = Radiobox(&themes_, &theme_idx_);
            slider_   = Slider("temperature", &temperature_, 0, 100, 1);
            button_ = action_button(
                "Close (Esc)", [&] { controller_.close_modal(); });

            Components children;
            for (auto& c : checks) {
                checks_.push_back(c);
                children.push_back(c);
            }
            children.push_back(radiobox_);
            children.push_back(slider_);
            children.push_back(button_);

            container_ = Container::Vertical(children);
            Add(container_);
        }

        Element OnRender() override
        {
            Elements cap_lines;
            cap_lines.push_back(section_title("Capabilities", Color::GrayLight));
            for (auto& c : checks_) {
                cap_lines.push_back(c->Render());
            }

            Element body = vbox({
                hbox({
                    text("ursa") | bold | color(Color::White),
                    text("  ·  interactive demo") | color(Color::GrayLight),
                    filler(),
                }),
                separatorEmpty(),
                vbox(std::move(cap_lines)),
                separatorEmpty(),
                section_title("Theme", Color::GrayLight),
                radiobox_->Render(),
                separatorEmpty(),
                section_title("Sampling", Color::GrayLight),
                slider_->Render(),
                separatorEmpty(),
                button_->Render() | center,
                separatorEmpty(),
                text("arrows navigate · Esc close") | dim | center,
            });

            return vbox({ separatorEmpty(), std::move(body), separatorEmpty() })
                | xflex;
        }

    private:
        Controller& controller_;

        std::vector<std::pair<std::string, bool>> capabilities_;
        std::vector<Component> checks_;

        std::vector<std::string> themes_;
        int theme_idx_ = 0;

        int temperature_ = 70;

        Component radiobox_;
        Component slider_;
        Component button_;
        Component container_;
    };

} // namespace

ftxui::Component make_settings(Controller& controller)
{
    return ftxui::Make<SettingsImpl>(controller);
}

} // namespace ursa
