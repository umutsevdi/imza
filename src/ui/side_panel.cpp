#include "ui.h"

#include "environment.h"

#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include <string>
#include <vector>

namespace ursa {
using namespace ftxui;
class SidePanel : public ComponentBase {
public:
    SidePanel(std::shared_ptr<Session> session, LayoutFn layout)
        : session_(std::move(session))
        , layout_(std::move(layout))
        , workspace_subscription_(
              get_environment()->subscribe_to_workspace_change(
                  [] { animation::RequestAnimationFrame(); }))
        , repository_subscription_(
              get_environment()->subscribe_to_repository_change(
                  [] { animation::RequestAnimationFrame(); }))
    {
    }

    ~SidePanel() override
    {
        repository_subscription_();
        workspace_subscription_();
    }

    Element OnRender() override
    {
        const LayoutCtx ctx = layout_();
        const bool narrow   = ctx.kind == LayoutCtx::Kind::NARROW;
        Elements parts;
        if (session_->todo().items.size()) {
            parts.push_back(render_todo(session_->todo(), ctx) | yflex);
        }

        if (!narrow) {
            auto env                = get_environment();
            const int global_skills = static_cast<int>(env->global_skills());
            const auto repository   = env->repository();
            if (repository && !repository->changed_files.empty()) {
                parts.push_back(
                    render_changed_files(repository->changed_files, ctx)
                    | yflex);
            }
            const std::optional<std::string> rules = env->agent_rules_path();
            if (rules) {
                parts.push_back(text(" " + *rules + " ") | bold
                    | color(Color::Black) | bgcolor(Color::Yellow) | xflex);
            }
            const int project_skills = static_cast<int>(env->project_skills());
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
    LayoutFn layout_;
    std::function<void()> workspace_subscription_;
    std::function<void()> repository_subscription_;
};

ftxui::Component make_side_panel(
    std::shared_ptr<Session> session, LayoutFn layout)
{
    return ftxui::Make<SidePanel>(std::move(session), std::move(layout));
}

namespace {

    struct ChangedFileStyle {
        std::string_view symbol;
        Color color;
    };

    ChangedFileStyle changed_file_style(ChangedFile::Kind kind)
    {
        switch (kind) {
        case ChangedFile::Kind::MODIFIED: return { "●", Color::YellowLight };
        case ChangedFile::Kind::ADDED: return { "+", Color::GreenLight };
        case ChangedFile::Kind::UNTRACKED: return { "?", Color::CyanLight };
        case ChangedFile::Kind::DELETED: return { "−", Color::RedLight };
        case ChangedFile::Kind::RENAMED: return { "→", Color::CyanLight };
        case ChangedFile::Kind::COPIED: return { "⧉", Color::CyanLight };
        case ChangedFile::Kind::CONFLICTED: return { "!", Color::RedLight };
        case ChangedFile::Kind::UNKNOWN: return { "•", PANEL_FG_DIM };
        }
        return { "•", PANEL_FG_DIM };
    }

    Element changed_file_item(const ChangedFile& file)
    {
        const ChangedFileStyle style = changed_file_style(file.kind);
        return hbox({
            text(std::string(style.symbol)) | bold | color(style.color),
            text(" "),
            paragraph(file.path) | color(PANEL_FG) | xflex,
        });
    }

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

Element render_changed_files(
    const std::vector<ChangedFile>& files, const LayoutCtx&)
{
    Elements rows;
    for (const ChangedFile& file : files) {
        rows.push_back(changed_file_item(file));
    }
    Element body = vbox(std::move(rows)) | borderStyled(ROUNDED, PANEL_BORDER);
    return vbox({ section_title("Changed files"), std::move(body) });
}

} // namespace ursa
