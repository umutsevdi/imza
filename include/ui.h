#pragma once

#include <ftxui/component/component_base.hpp>

#include <functional>
#include <string>

#include "agent.h"

namespace ursa {

int run_repl(const Config& cfg);

ftxui::Component make_chat(Controller& controller, std::function<int()> width);
ftxui::Component make_side_panel(Controller& controller);
ftxui::Component make_settings(Controller& controller);
ftxui::Component make_modal(Controller& controller);

} // namespace ursa
