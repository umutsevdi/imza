#include "app/application_state.h"
#include "app/flows.h"
#include "permissions/store.h"
#include "tools/mcp_manager.h"
#include "ui/ui.h"

#include "tools/skills.h"
#include "workspace/review.h"

#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <atomic>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace imza {
using namespace ftxui;

namespace {

    Element titled_section(std::string_view title, Element body)
    {
        return vbox({ section_title(std::string(title)), std::move(body) });
    }

    Element titled_box(std::string_view title, Elements rows)
    {
        return titled_section(
            title, vbox(std::move(rows)) | borderStyled(ROUNDED, PANEL_BORDER));
    }

    Element changed_file_item(const ChangedFile& file);

    Element changed_files_panel(Elements rows, const ChangeSummary& changes)
    {
        Element body
            = vbox(std::move(rows)) | borderStyled(ROUNDED, PANEL_BORDER);
        Element title = hbox({ section_title("Changed files"), filler(),
            diffstat_chip(changes.additions, changes.deletions) });
        return vbox({ std::move(title), std::move(body) });
    }

    std::string changed_file_target(const ChangedFile& file)
    {
        if (file.kind != ChangedFile::Kind::RENAMED
            && file.kind != ChangedFile::Kind::COPIED) {
            return file.path;
        }
        const std::size_t arrow = file.path.rfind(" -> ");
        return arrow == std::string::npos ? file.path
                                          : file.path.substr(arrow + 4);
    }

} // namespace

class SidePanel : public ComponentBase {
public:
    SidePanel(std::shared_ptr<ApplicationState> state, LayoutFn layout,
        WorkflowFn workflow, WorkflowNavigateFn navigate)
        : _state(std::move(state))
        , _layout(std::move(layout))
        , _workflow(std::move(workflow))
        , _navigate(std::move(navigate))
        , _workspace_subscription(
              _state->environment->subscribe_to_workspace_change(
                  [] { animation::RequestAnimationFrame(); }))
        , _repository_subscription(
              _state->environment->subscribe_to_repository_change(
                  [] { animation::RequestAnimationFrame(); }))
        , _attachments_subscription(
              _state->session->subscribe_to_attachments_change([this] {
                  _attachments_dirty.store(true);
                  animation::RequestAnimationFrame();
              }))
        , _review_subscription(_state->review ? _state->review->subscribe([] {
            animation::RequestAnimationFrame();
        })
                                              : Signal<>::Subscription { })
        , _permission_subscription(
              _state->permissions->subscribe_to_grants_change(
                  [state = std::weak_ptr<ApplicationState>(_state)] {
                      if (const auto current = state.lock()) {
                          current->post(
                              [] { animation::RequestAnimationFrame(); });
                      }
                  }))
        , _update_subscription(_state->environment->subscribe_to_update_change(
              [state = std::weak_ptr<ApplicationState>(_state)] {
                  if (const auto current = state.lock()) {
                      current->post([] { animation::RequestAnimationFrame(); });
                  }
              }))
    {
        _links_container = Container::Vertical({ });
        Add(_links_container);
    }

    Element OnRender() override
    {
        const LayoutCtx ctx = _layout();
        const bool narrow   = ctx.kind == LayoutCtx::Kind::NARROW;
        _active_links.clear();
        Elements parts;
        _append_plans(parts);
        _append_review_comments(parts);
        const auto todo = _state->session->todo();
        if (!todo->items.empty()) {
            parts.push_back(render_todo(*todo, ctx) | yflex);
        }
        if (!narrow) {
            const auto& env       = _state->environment;
            const auto repository = env->repository();
            if (repository && !repository->changed_files.empty()) {
                parts.push_back(_render_changed_files(*repository) | yflex);
            }
            if (_attachments_dirty.exchange(false)) {
                _attachment_names = _state->session->attachment_names();
            }
            const auto [project, global]
                = _state->skills->counts(_state->environment->skills());
            std::vector<std::string> mcp_servers;
            if (_state->mcp != nullptr) {
                for (const auto& server : _state->mcp->snapshot()) {
                    if (server.state == McpServerState::DISABLED) {
                        continue;
                    }
                    std::string line = server.id;
                    if (server.state == McpServerState::CONNECTED) {
                        line += " · " + std::to_string(server.tool_count)
                            + " tools";
                    } else if (server.state == McpServerState::FAILED) {
                        line += " ✗";
                    }
                    mcp_servers.push_back(std::move(line));
                }
            }
            parts.push_back(render_context_box(env->agent_rules_path(),
                _attachment_names, project, global, mcp_servers));
            const PermissionStore::Snapshot grants
                = _state->permissions->snapshot();
            PermissionView permissions
                = make_permission_view(_state->runtime_flags, *grants);
            if (has_custom_permissions(permissions)) {
                parts.push_back(render_permissions_box(permissions));
            }
        }
        if (const std::optional<std::string> update
            = _state->environment->update_available()) {
            parts.push_back(render_update_available(*update));
        }
        Element body = vbox(std::move(parts));
        if (narrow) {
            return panel(body) | xflex;
        }
        return panel(body) | size(WIDTH, EQUAL, LayoutCtx::LEFT_WIDTH);
    }

    bool OnEvent(Event event) override
    {
        for (const Component& link : _active_links) {
            if (link->OnEvent(event)) {
                return true;
            }
        }
        return false;
    }

private:
    template <typename Payload> struct Link {
        std::shared_ptr<Payload> data;
        Component component;
    };

    template <typename Key, typename Payload, typename Render, typename Click,
        typename Update>
    Component _memoized_link(std::map<Key, Link<Payload>>& cache, Key key,
        std::shared_ptr<Payload> payload, Render render, Click on_click,
        Update update)
    {
        auto found = cache.find(key);
        if (found == cache.end()) {
            Component component = inline_link_button(
                [payload, render] { return render(*payload); },
                std::move(on_click), PANEL_FG_DIM);
            _links_container->Add(component);
            found = cache
                        .emplace(std::move(key),
                            Link<Payload> { std::move(payload), component })
                        .first;
        } else {
            update(*found->second.data);
        }
        _active_links.push_back(found->second.component);
        return found->second.component;
    }

    // Plans widget: the newest plan carries the live marker, superseded
    // ones stay dim; each row opens the plan in the viewer modal.
    void _append_plans(Elements& parts)
    {
        const auto plans = _state->session->plans();
        if (plans->empty()) {
            _plan_links.clear();
            return;
        }
        Elements rows;
        for (std::size_t index = 0; index < plans->size(); ++index) {
            const bool latest = index + 1 == plans->size();
            rows.push_back(
                _plan_link(index, (*plans)[index].content, latest)->Render());
        }
        parts.push_back(titled_box("Plans", std::move(rows)));
    }

    Element _render_changed_files(const RepositoryState& repository)
    {
        Elements rows;
        for (const ChangedFile& file : repository.changed_files) {
            rows.push_back(_changed_file_link(file)->Render());
        }
        return changed_files_panel(std::move(rows), repository.changes);
    }

    void _append_review_comments(Elements& parts)
    {
        if (_workflow() != WorkflowPhase::REVIEW || !_state->review) {
            return;
        }
        const std::vector<ReviewComment> comments = _state->review->comments();
        if (comments.empty()) {
            return;
        }
        Elements rows;
        for (const ReviewComment& comment : comments) {
            const std::string line = comment.anchor.new_line
                ? std::to_string(*comment.anchor.new_line)
                : comment.anchor.old_line
                ? std::to_string(*comment.anchor.old_line)
                : "?";
            std::filesystem::path path(comment.anchor.file);
            const std::string label = path.filename().string() + ":" + line
                + (comment.stale ? "  stale" : "");
            rows.push_back(
                hbox({ _comment_link(comment.id, label)->Render(), filler(),
                    text(fit(comment.body, LayoutCtx::LEFT_WIDTH / 2 - 6))
                        | color(PANEL_FG_DIM) })
                | xflex);
        }
        parts.push_back(titled_box("Review Comments", std::move(rows)));
    }

    Component _changed_file_link(const ChangedFile& file)
    {
        return _memoized_link(
            _file_links, file.path, std::make_shared<ChangedFile>(file),
            [](const ChangedFile& current) {
                return changed_file_item(current);
            },
            [this, target = changed_file_target(file)] {
                _state->review->request_file_jump(target);
                _navigate(WorkflowPhase::REVIEW);
            },
            [&file](ChangedFile& existing) { existing = file; });
    }

    Component _plan_link(
        std::size_t index, const std::string& content, bool latest)
    {
        auto payload = std::make_shared<std::string>(content);
        return _memoized_link(
            _plan_links, index, payload,
            [index, latest](const std::string&) {
                // Explicit colors are only needed on the latest row: the
                // link wrapper already paints inactive rows dim-colored,
                // and | dim keeps them greyed on limited palettes.
                Element row = hbox({
                    text("●") | color(latest ? HL_GREEN : PANEL_FG_DIM),
                    text(" "),
                    paragraph(plan_revision_label(index))
                        | color(latest ? PANEL_FG : PANEL_FG_DIM) | xflex,
                });
                return latest ? row : row | dim;
            },
            [this, index, payload] {
                ViewerModal vm { plan_revision_label(index), *payload, "md",
                    1 };
                vm.line_numbers = false;
                enqueue_user_modal(*_state, vm);
            },
            [content](std::string& existing) { existing = content; });
    }

    Component _comment_link(std::size_t id, std::string label)
    {
        return _memoized_link(
            _comment_links, id, std::make_shared<std::string>(label),
            [](const std::string& current) { return text(current) | bold; },
            [this, id] { _state->review->request_jump(id); },
            [&label](std::string& existing) { existing = std::move(label); });
    }

    std::shared_ptr<ApplicationState> _state;
    LayoutFn _layout;
    WorkflowFn _workflow;
    WorkflowNavigateFn _navigate;
    Signal<>::Subscription _workspace_subscription;
    Signal<>::Subscription _repository_subscription;
    std::atomic<bool> _attachments_dirty { true };
    std::vector<std::string> _attachment_names;
    Signal<>::Subscription _attachments_subscription;
    Signal<>::Subscription _review_subscription;
    Signal<>::Subscription _permission_subscription;
    Signal<>::Subscription _update_subscription;
    Component _links_container;
    std::map<std::string, Link<ChangedFile>> _file_links;
    std::map<std::size_t, Link<std::string>> _plan_links;
    std::map<std::size_t, Link<std::string>> _comment_links;
    std::vector<Component> _active_links;
};

ftxui::Component make_side_panel(std::shared_ptr<ApplicationState> state,
    LayoutFn layout, WorkflowFn workflow, WorkflowNavigateFn navigate)
{
    return ftxui::Make<SidePanel>(std::move(state), std::move(layout),
        std::move(workflow), std::move(navigate));
}

namespace {

    struct ChangedFileStyle {
        std::string_view symbol;
        Color color;
    };

    ChangedFileStyle changed_file_style(ChangedFile::Kind kind)
    {
        switch (kind) {
        case ChangedFile::Kind::MODIFIED: return { "●", HL_YELLOW };
        case ChangedFile::Kind::ADDED: return { "+", HL_GREEN };
        case ChangedFile::Kind::UNTRACKED: return { "?", HL_CYAN };
        case ChangedFile::Kind::DELETED: return { "−", HL_RED };
        case ChangedFile::Kind::RENAMED: return { "→", HL_CYAN };
        case ChangedFile::Kind::COPIED: return { "⧉", HL_CYAN };
        case ChangedFile::Kind::CONFLICTED: return { "!", HL_RED };
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
    return titled_section("Tasks", std::move(body));
}

Element render_context_box(const std::optional<std::string>& rules,
    const std::vector<std::string>& attachments, SkillCounts project_skills,
    SkillCounts global_skills, const std::vector<std::string>& mcp_servers)
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
            text(std::format(
                "{}/{}", project_skills.active, project_skills.total))
                | color(PANEL_FG) | dim,
        }));
    }
    if (global_skills.total > 0) {
        context_box.push_back(hbox({
            text("Global Skills") | bold | color(PANEL_FG) | xflex,
            text(
                std::format("{}/{}", global_skills.active, global_skills.total))
                | color(PANEL_FG) | dim,
        }));
    }
    if (!mcp_servers.empty()) {
        context_box.push_back(hbox({
            text("MCP") | bold | color(PANEL_FG) | xflex,
            text(join(mcp_servers, ", ")) | color(PANEL_FG) | dim,
        }));
    }
    if (!context_box.empty()) {
        Element body = vbox(std::move(context_box))
            | borderStyled(ROUNDED, PANEL_BORDER);
        return titled_section("Context", std::move(body));
    }
    return vbox();
}

PermissionView make_permission_view(
    RuntimeFlag flags, const PermissionStore::Grants& grants)
{
    PermissionView view;
    view.web_disabled   = (flags & RuntimeFlag::WEB) == RuntimeFlag::NONE;
    view.shell_disabled = (flags & RuntimeFlag::SHELL) == RuntimeFlag::NONE;
    view.approvals_skipped
        = (flags & RuntimeFlag::SKIP_PERMISSIONS) != RuntimeFlag::NONE;

    for (const PermissionGrant& grant : grants) {
        if (const auto* directory = std::get_if<ExternalGrant>(&grant)) {
            view.folders.push_back(directory->string());
            continue;
        }
        if (std::holds_alternative<SkillGrant>(grant)) {
            continue;
        }
        const auto& command = std::get<ShellCommandGrant>(grant);
        std::string label
            = command.program + " " + command.subcommand.value_or("*");
        view.commands.push_back(std::move(label));
    }
    const auto sort_unique = [](std::vector<std::string>& values) {
        std::ranges::sort(values);
        values.erase(std::unique(values.begin(), values.end()), values.end());
    };
    sort_unique(view.folders);
    sort_unique(view.commands);
    return view;
}

bool has_custom_permissions(const PermissionView& view)
{
    return view.web_disabled || view.shell_disabled || view.approvals_skipped
        || !view.folders.empty() || !view.commands.empty();
}

Element render_permissions_box(const PermissionView& view)
{
    Elements rows;
    const auto append_value = [&rows](std::string label, std::string value) {
        rows.push_back(
            hbox({ text(std::move(label)) | bold | color(PANEL_FG) | xflex,
                text(std::move(value)) | color(PANEL_FG) | dim }));
    };
    const auto append_values = [&rows](std::string label,
                                   const std::vector<std::string>& values) {
        if (values.empty()) {
            return;
        }
        Elements rendered;
        for (const std::string& value : values) {
            rendered.push_back(paragraph(value) | color(PANEL_FG) | dim);
        }
        rows.push_back(
            hbox({ text(std::move(label)) | bold | color(PANEL_FG) | xflex,
                vbox(std::move(rendered)) }));
    };
    if (view.web_disabled) {
        append_value("Web", "disabled");
    }
    if (view.shell_disabled) {
        append_value("Shell", "disabled");
    }
    if (view.approvals_skipped) {
        append_value("Approvals", "skipped");
    }
    append_values("Folders", view.folders);
    append_values("Commands", view.commands);
    if (rows.empty()) {
        return vbox();
    }
    Element body = vbox(std::move(rows)) | borderStyled(ROUNDED, PANEL_BORDER);
    return titled_section("Permissions", std::move(body));
}

Element render_update_available(std::string version)
{
    return hbox({
        text(" Update Available ") | bold | color(PANEL_FG),
        filler(),
        text("  v" + std::move(version) + " ") | bold | color(PANEL_COLOR_FOCUS)
            | bgcolor(HL_GREEN),
    });
}

} // namespace imza
