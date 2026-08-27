#include "command.h"
#include "ui.h"

#include "environment.h"

#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ursa {
using namespace ftxui;
class SidePanel : public ComponentBase {
public:
    SidePanel(std::shared_ptr<Session> session, Controller& controller,
        LayoutFn layout)
        : session_(std::move(session))
        , controller_(controller)
        , layout_(std::move(layout))
    {
        changed_files_checker_
            = std::jthread([this](const std::stop_token& stop) {
                  while (!stop.stop_requested()) {
                      auto ws = workspace_.load();
                      if (ws) {
                          std::string out = run_command(
                              "git status --short", std::chrono::seconds { 1 })
                                                .output;
                          {
                              std::unique_lock g { display_mutex_ };
                              if (out != display_) {
                                  display_ = out;
                                  animation::RequestAnimationFrame();
                              }
                          }
                      }
                      std::this_thread::sleep_for(std::chrono::seconds { 2 });
                  }
              });
    };

    Element OnRender() override
    {
        const LayoutCtx ctx = layout_();
        const bool narrow   = ctx.kind == LayoutCtx::Kind::NARROW;
        Elements parts;
        if (session_->todo().items.size()) {
            parts.push_back(render_todo(session_->todo(), ctx) | yflex);
        }

        if (!narrow) {
            auto env = get_environment();
            if (env->ready() && !subscription_started_) {
                subscription_started_ = true;
                env->subscribe_to_workspace_change(
                    [this](const std::shared_ptr<const WorkspaceEnvironment>&
                            wsenv) { workspace_ = (wsenv); });
            }

            {
                std::shared_lock g { display_mutex_ };
                if (!display_.empty()) {
                    parts.push_back(
                        render_changed_files(display_, ctx) | yflex);
                }
            }

            const std::optional<std::string>& rules
                = get_environment()->agent_rules_path();
            if (rules) {
                parts.push_back(text(" " + *rules + " ") | bold
                    | color(Color::Black) | bgcolor(Color::Yellow) | xflex);
            }
            const int project_skills
                = static_cast<int>(get_environment()->project_skills());
            const int global_skills
                = static_cast<int>(get_environment()->global_skills());
            if (project_skills > 0) {
                parts.push_back(
                    text(std::format(" {} Project Skills ", project_skills))
                    | bold | color(Color::Black) | bgcolor(Color::BlueLight)
                    | xflex);
            }
            if (global_skills > 0) {
                parts.push_back(
                    text(std::format(" {} Global Skills ", global_skills))
                    | bold | color(Color::Black) | bgcolor(Color::GrayLight)
                    | xflex);
            }
        }
        Element body = vbox(std::move(parts));
        if (narrow) {
            return body | xflex;
        }
        return panel(body) | size(WIDTH, EQUAL, LayoutCtx::panel_width);
    }

private:
    std::shared_ptr<Session> session_;
    Controller& controller_;
    std::shared_mutex display_mutex_;
    std::string display_;
    LayoutFn layout_;
    std::atomic<bool> subscription_started_ { false };
    std::atomic<std::shared_ptr<const WorkspaceEnvironment>> workspace_;
    std::jthread changed_files_checker_;
};

ftxui::Component make_side_panel(
    std::shared_ptr<Session> session, Controller& controller, LayoutFn layout)
{
    return ftxui::Make<SidePanel>(
        std::move(session), controller, std::move(layout));
}

namespace {

    Element changed_file_item(const ChangedFile& f)
    {
        Color c = Color::GrayDark;
        if (f.status == "M") {
            c = Color::YellowLight;
        } else if (f.status == "A") {
            c = Color::GreenLight;
        } else if (f.status == "D") {
            c = Color::RedLight;
        }
        return hbox({
            text(f.status) | color(c) | bold,
            text(" "),
            paragraph(f.path) | xflex,
        });
    }

} // namespace

namespace {

    Element todo_item(const TodoItem& it)
    {
        using Status            = TodoItem::Status;
        ftxui::Color mark_color = PANEL_FG_DIM;
        bool mark_bold          = false;
        std::string mark;
        switch (it.status) {
        case Status::IN_PROGRESS:
            mark       = "→";
            mark_color = PANEL_FG;
            mark_bold  = true;
            break;
        case Status::COMPLETED: mark = "[x]"; break;
        case Status::CANCELLED: mark = "[-]"; break;
        case Status::PENDING: mark = "[ ]"; break;
        }
        const bool inactive
            = it.status == Status::COMPLETED || it.status == Status::CANCELLED;
        Element mark_el = text(mark) | color(mark_color);
        if (mark_bold) {
            mark_el = std::move(mark_el) | bold;
        }
        Element content
            = inactive ? dim(paragraph(it.content)) : paragraph(it.content);
        return hbox(
            { std::move(mark_el), text(" "), std::move(content) | xflex });
    }

} // namespace

Element render_todo(const TodoList& todo, const LayoutCtx&)
{
    Elements parts;
    for (const auto& it : todo.items) {
        parts.push_back(todo_item(it));
    }
    Element body = parts.empty()
        ? dim(text("none"))
        : vbox(std::move(parts)) | borderStyled(ROUNDED, PANEL_BORDER);
    return vbox({ section_title("Todo"), std::move(body) });
}

Element render_changed_files(const std::string& files, const LayoutCtx&)
{
    Element body
        = vbox(dim(paragraph(files)) | borderStyled(ROUNDED, PANEL_BORDER));
    return vbox({ section_title("Changed files"), std::move(body) });
}

} // namespace ursa
