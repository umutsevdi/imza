#include "app/flows.h"
#include "ui/ui.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <mutex>
#include <print>
#include <string>
#include <thread>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace imza {

namespace {

    bool stdout_is_tty()
    {
#ifdef _WIN32
        return _isatty(_fileno(stdout)) != 0;
#else
        return isatty(STDOUT_FILENO) != 0;
#endif
    }

    class UpdateScreen final : public ftxui::ComponentBase {
    public:
        UpdateScreen(const std::atomic_bool& done, std::function<void()> poke,
            std::function<ftxui::Element()> body)
            : _done(done)
            , _poke(std::move(poke))
            , _body(std::move(body))
        {
            Add(ftxui::Renderer([this] { return _body(); }));
            using namespace std::chrono_literals;
            _ticker = std::thread([this] {
                while (!_done.load()) {
                    _poke();
                    std::this_thread::sleep_for(50ms);
                }
            });
        }

        ~UpdateScreen() override
        {
            if (_ticker.joinable()) {
                _ticker.join();
            }
        }

    private:
        const std::atomic_bool& _done;
        std::function<void()> _poke;
        std::function<ftxui::Element()> _body;
        std::thread _ticker;
    };

} // namespace

std::string render_markdown_to_text(std::string_view md, int width)
{
    using namespace ftxui;
    Element document = render_markdown_element(md, width);
    auto screen
        = Screen::Create(Dimension::Fixed(width), Dimension::Fit(document));
    Render(screen, document);
    return screen.ToString();
}

void print_markdown_output(std::string_view md)
{
    if (!stdout_is_tty() || md.empty()) {
        std::println("{}", md);
        return;
    }
    const int term_w = ftxui::Terminal::Size().dimx;
    std::cout << render_markdown_to_text(md, std::max(40, term_w)) << std::endl;
}

int run_update_display()
{
    const auto println = [](std::string line) { std::println("{}", line); };
    if (!stdout_is_tty()) {
        const PreparedUpdate update = prepare_update(println);
        return run_prepared_update(update, println);
    }

    std::atomic_bool done { false };
    std::mutex stage_mutex;
    std::string stage = "Checking for the latest imza release...";
    int frame         = 0;

    ftxui::ScreenInteractive screen
        = ftxui::ScreenInteractive::FullscreenAlternateScreen();
    auto component = ftxui::Make<UpdateScreen>(
        done,
        [&screen, &frame] {
            ++frame;
            screen.PostEvent(ftxui::Event::Custom);
        },
        [&stage_mutex, &stage, &frame]() -> ftxui::Element {
            std::lock_guard lock(stage_mutex);
            return ftxui::vbox(
                { ftxui::text("imza update") | ftxui::bold, ftxui::text(""),
                    ftxui::hbox({ dim_spinner(frame), ftxui::text(" "),
                        ftxui::text(stage) }) });
        });

    // sudo reads its password from the terminal, which the spinner's
    // event loop owns until it exits.
    PreparedUpdate update;
    std::thread worker([&] {
        update = prepare_update([&stage_mutex, &stage](std::string line) {
            std::lock_guard lock(stage_mutex);
            stage = std::move(line);
        });
        done.store(true);
        screen.Exit();
    });

    screen.Loop(component);
    worker.join();
    std::println("{}", stage);
    return run_prepared_update(update, println);
}

} // namespace imza