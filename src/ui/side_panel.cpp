#include "app/application_state.h"
#include "app/flows.h"
#include "permissions/permission_store.h"
#include "runtime/mcp_subsystem.h"
#include "ui/ui.h"
#include "workspace/review_subsystem.h"

#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <algorithm>
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
        , _sidebar(_state->sidebar)
        , _workspace_subscription(
              _state->environment->subscribe_to_workspace_change(
                  [] { animation::RequestAnimationFrame(); }))
        , _system_subscription(_state->environment->subscribe_to_system_change(
              [] { animation::RequestAnimationFrame(); }))
        , _repository_subscription(
              _state->environment->subscribe_to_repository_change(
                  [] { animation::RequestAnimationFrame(); }))
        , _attachments_subscription(
              _state->session->subscribe_to_attachments_change([this] {
                  _attachments_dirty.store(true);
                  animation::RequestAnimationFrame();
              }))
        , _review_subscription(_state->review
                  ? _state->review->subscribe_to_change(
                        [] { animation::RequestAnimationFrame(); })
                  : Signal<>::Subscription { })
        , _skills_subscription(_state->session->subscribe_to_skills_change(
              [state = std::weak_ptr<ApplicationState>(_state)] {
                  if (const auto current = state.lock()) {
                      current->post([] { animation::RequestAnimationFrame(); });
                  }
              }))
        , _permission_subscription(
              _state->permissions->subscribe_to_grants_change(
                  [state = std::weak_ptr<ApplicationState>(_state)] {
                      if (const auto current = state.lock()) {
                          current->post(
                              [] { animation::RequestAnimationFrame(); });
                      }
                  }))
        , _skip_subscription(_state->permissions->subscribe_to_skip_change(
              [state = std::weak_ptr<ApplicationState>(_state)] {
                  if (const auto current = state.lock()) {
                      current->post([] { animation::RequestAnimationFrame(); });
                  }
              }))
        , _documents_subscription(
              _state->session->subscribe_to_documents_change(
                  [state = std::weak_ptr<ApplicationState>(_state)] {
                      if (const auto current = state.lock()) {
                          current->post(
                              [] { animation::RequestAnimationFrame(); });
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
        if (_sidebar.documents) {
            _append_documents(parts);
        } else {
            _document_links.clear();
        }
        if (_sidebar.review_comments) {
            _append_review_comments(parts);
        }
        const auto todo = _state->session->todo();
        if (_sidebar.tasks && !todo->items.empty()) {
            parts.push_back(render_todo(*todo, ctx) | yflex);
        }
        if (!narrow) {
            const auto& env       = _state->environment;
            const auto repository = env->repository();
            if (_sidebar.changed_files && repository
                && !repository->changed_files.empty()) {
                parts.push_back(_render_changed_files(*repository) | yflex);
            }
            if (_attachments_dirty.exchange(false)) {
                _attachment_names = _state->session->attachment_names();
            }
            if (_sidebar.context) {
                std::vector<ContextSkill> skills;
                for (const Skill& skill : env->skills()) {
                    skills.push_back({ skill.name,
                        _state->session->is_skill_loaded(skill.path) });
                }
                std::vector<std::string> mcp_servers;
                if (_state->mcp != nullptr) {
                    for (const auto& server : _state->mcp->snapshot()) {
                        if (server.state != McpServerState::CONNECTED) {
                            continue;
                        }
                        mcp_servers.push_back(server.id + " · "
                            + std::to_string(server.tool_count) + " tools");
                    }
                }
                parts.push_back(render_context_box(env->agent_rules_path(),
                    _attachment_names, skills, mcp_servers));
            }
            if (_sidebar.permissions) {
                const PermissionStore::Snapshot grants
                    = _state->permissions->snapshot();
                PermissionView permissions
                    = make_permission_view(_state->runtime_flags,
                        _state->permissions->skip_permissions(), *grants);
                if (has_custom_permissions(permissions)) {
                    parts.push_back(render_permissions_box(permissions));
                }
            }
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
            // Non-focusable: a click must not rotate the app-level Stacked
            // container away from the main column.
            Component component
                = hover_link([payload, render] { return render(*payload); },
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

    // Documents widget: plan revisions and build logs in session order.
    // The newest document carries the live marker, superseded ones stay
    // dim; each row opens the document in the viewer modal.
    void _append_documents(Elements& parts)
    {
        const auto documents = _state->session->documents();
        if (documents->empty()) {
            _document_links.clear();
            return;
        }
        const std::vector<std::string> labels = document_labels(*documents);
        Elements rows;
        for (std::size_t index = 0; index < documents->size(); ++index) {
            const bool latest = index + 1 == documents->size();
            rows.push_back(_document_link(
                index, labels[index], (*documents)[index].content, latest)
                    ->Render());
        }
        parts.push_back(titled_box("Documents", std::move(rows)));
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

    Component _document_link(std::size_t index, std::string label,
        const std::string& content, bool latest)
    {
        auto payload = std::make_shared<std::string>(content);
        return _memoized_link(
            _document_links, index, payload,
            [label, latest](const std::string&) {
                // Explicit colors are only needed on the latest row: the
                // link wrapper already paints inactive rows dim-colored,
                // and | dim keeps them greyed on limited palettes.
                Element row = hbox({
                    text("●") | color(latest ? HL_GREEN : PANEL_FG_DIM),
                    text(" "),
                    paragraph(label) | color(latest ? PANEL_FG : PANEL_FG_DIM)
                        | xflex,
                });
                return latest ? row : row | dim;
            },
            [this, label, payload] {
                ViewerModal vm { label, *payload, "md", 1 };
                vm.line_numbers = false;
                vm.save_name    = label;
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
    SidebarConfig _sidebar;
    Signal<>::Subscription _workspace_subscription;
    Signal<>::Subscription _system_subscription;
    Signal<>::Subscription _repository_subscription;
    std::atomic<bool> _attachments_dirty { true };
    std::vector<std::string> _attachment_names;
    Signal<>::Subscription _attachments_subscription;
    Signal<>::Subscription _review_subscription;
    Signal<>::Subscription _skills_subscription;
    Signal<>::Subscription _permission_subscription;
    Signal<>::Subscription _skip_subscription;
    Signal<>::Subscription _documents_subscription;
    Component _links_container;
    std::map<std::string, Link<ChangedFile>> _file_links;
    std::map<std::size_t, Link<std::string>> _document_links;
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
    const std::vector<std::string>& attachments,
    const std::vector<ContextSkill>& skills,
    const std::vector<std::string>& mcp_servers)
{
    Elements context_box;
    const auto append_values = [&context_box](std::string label,
                                   const std::vector<std::string>& values) {
        if (values.empty()) {
            return;
        }
        Elements rendered;
        for (const std::string& value : values) {
            rendered.push_back(paragraph(value) | color(PANEL_FG) | dim);
        }
        context_box.push_back(
            hbox({ text(std::move(label)) | bold | color(PANEL_FG) | xflex,
                vbox(std::move(rendered)) }));
    };

    if (rules || !attachments.empty()) {
        std::vector<std::string> files;
        if (rules) {
            files.push_back(*rules);
        }
        files.insert(files.end(), attachments.begin(), attachments.end());
        append_values("Files", files);
    }
    if (!skills.empty()) {
        std::vector<std::string> loaded_names;
        for (const ContextSkill& skill : skills) {
            if (skill.loaded) {
                loaded_names.push_back(skill.name);
            }
        }
        const std::string label
            = std::format("Skills {}/{}", loaded_names.size(), skills.size());
        if (loaded_names.empty()) {
            context_box.push_back(
                hbox({ text(label) | bold | color(PANEL_FG) | xflex,
                    text("none") | color(PANEL_FG) | dim }));
        } else {
            append_values(label, loaded_names);
        }
    }
    append_values("MCP", mcp_servers);
    if (!context_box.empty()) {
        Element body = vbox(std::move(context_box))
            | borderStyled(ROUNDED, PANEL_BORDER);
        return titled_section("Context", std::move(body));
    }
    return vbox();
}

PermissionView make_permission_view(RuntimeFlag flags, bool approvals_skipped,
    const PermissionStore::Grants& grants)
{
    PermissionView view;
    view.web_disabled      = (flags & RuntimeFlag::WEB) == RuntimeFlag::NONE;
    view.shell_disabled    = (flags & RuntimeFlag::SHELL) == RuntimeFlag::NONE;
    view.approvals_skipped = approvals_skipped;

    for (const PermissionGrant& grant : grants) {
        if (const auto* directory = std::get_if<ExternalGrant>(&grant)) {
            view.folders.push_back(directory->string());
            continue;
        }
        if (std::holds_alternative<SkillGrant>(grant)) {
            continue;
        }
        if (const auto* mcp = std::get_if<McpGrant>(&grant)) {
            view.mcp_servers.push_back(mcp->server_id);
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
    sort_unique(view.mcp_servers);
    return view;
}

bool has_custom_permissions(const PermissionView& view)
{
    return view.web_disabled || view.shell_disabled || view.approvals_skipped
        || !view.folders.empty() || !view.commands.empty()
        || !view.mcp_servers.empty();
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
    append_values("MCP", view.mcp_servers);
    if (rows.empty()) {
        return vbox();
    }
    Element body = vbox(std::move(rows)) | borderStyled(ROUNDED, PANEL_BORDER);
    return titled_section("Permissions", std::move(body));
}

} // namespace imza
