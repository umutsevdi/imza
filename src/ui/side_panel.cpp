#include "ui.h"

#include "environment.h"

#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include <atomic>
#include <string>
#include <vector>

namespace ursa {
using namespace ftxui;
class SidePanel : public ComponentBase {
public:
    SidePanel(std::shared_ptr<Session> session, Controller& controller, LayoutFn layout)
        : session_(std::move(session))
        , controller_(controller)
        , layout_(std::move(layout))
        , workspace_subscription_(
              get_environment()->subscribe_to_workspace_change(
                  [] { animation::RequestAnimationFrame(); }))
        , repository_subscription_(
              get_environment()->subscribe_to_repository_change(
                  [] { animation::RequestAnimationFrame(); }))
        , title_subscription_(session_->subscribe_to_title_change(
              [] { animation::RequestAnimationFrame(); }))
        , attachments_subscription_(
              session_->subscribe_to_attachments_change([this] {
                  attachments_dirty_.store(true);
                  animation::RequestAnimationFrame();
              }))
    {
    }

    Element OnRender() override
    {
        const LayoutCtx ctx = layout_();
        const bool narrow   = ctx.kind == LayoutCtx::Kind::NARROW;
        Elements parts;
        const std::string title = session_->title();
        parts.push_back(paragraph(title.empty() ? "New Session" : title) | bold
            | color(PANEL_FG));
        parts.push_back(separator());
        if (session_->todo().items.size()) {
            parts.push_back(render_todo(session_->todo(), ctx) | yflex);
        }

        if (!narrow) {
            if (attachments_dirty_.exchange(false)) {
                attachment_names_ = session_->attachment_names();
            }
            auto env              = get_environment();
            const auto repository = env->repository();
            if (repository && !repository->changed_files.empty()) {
                parts.push_back(
                    render_changed_files(repository->changed_files, ctx)
                    | yflex);
            };
            const auto [project, global] = controller_.skill_counts();
            parts.push_back(render_context_box(env->agent_rules_path(), attachment_names_, project, global));
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
    LayoutFn layout_;
    Signal<>::Subscription workspace_subscription_;
    Signal<>::Subscription repository_subscription_;
    Signal<>::Subscription title_subscription_;
    std::atomic<bool> attachments_dirty_ { true };
    std::vector<std::string> attachment_names_;
    Signal<>::Subscription attachments_subscription_;
};

ftxui::Component make_side_panel(
    std::shared_ptr<Session> session, Controller& controller, LayoutFn layout)
{
    return ftxui::Make<SidePanel>(std::move(session), controller, std::move(layout));
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

Element render_context_box(const std::optional<std::string>& rules,
    const std::vector<std::string>& attachments, SkillCounts project_skills,
    SkillCounts global_skills)
{
    Elements context_box;
    if (rules || !attachments.empty()) {
        Elements files;
        if (rules) {
            files.push_back(paragraph(*rules) | color(PANEL_FG) | dim);
        }
        for (const std::string& attachment : attachments) {
            files.push_back(paragraph(attachment) | color(PANEL_FG) | dim);
        }
        context_box.push_back(hbox({
            text("Files") | bold | color(PANEL_FG) | xflex,
            vbox(std::move(files)),
        }));
    }
    if (project_skills.total > 0) {
        context_box.push_back(hbox({
            text("Project Skills") | bold | color(PANEL_FG) | xflex,
            text(std::format("{}/{}", project_skills.active, project_skills.total)) | color(PANEL_FG) | dim,
        }));
    }
    if (global_skills.total > 0) {
        context_box.push_back(hbox({
            text("Global Skills") | bold | color(PANEL_FG) | xflex,
            text(std::format("{}/{}", global_skills.active, global_skills.total)) | color(PANEL_FG) | dim,
        }));
    }
    if (!context_box.empty()) {
        Element body = vbox(std::move(context_box))
            | borderStyled(ROUNDED, PANEL_BORDER);
        return vbox({ section_title("Context"), std::move(body) });
    }
    return vbox();
}

Element render_context_box(const std::optional<std::string>& rules,
    const std::vector<std::string>& attachments, int project_skills,
    int global_skills)
{
    return render_context_box(rules, attachments,
        SkillCounts { static_cast<std::size_t>(project_skills), static_cast<std::size_t>(project_skills) },
        SkillCounts { static_cast<std::size_t>(global_skills), static_cast<std::size_t>(global_skills) });
}

} // namespace ursa
//
