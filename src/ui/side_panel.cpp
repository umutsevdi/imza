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
#include <set>
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
              redraw_on_change(_state)))
        , _permission_subscription(
              _state->permissions->subscribe_to_grants_change(
                  redraw_on_change(_state)))
        , _skip_subscription(_state->permissions->subscribe_to_skip_change(
              redraw_on_change(_state)))
        , _documents_subscription(
              _state->session->subscribe_to_documents_change(
                  redraw_on_change(_state)))
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
            if (_sidebar.runtime) {
                const RuntimeRowLink link
                    = [this](RuntimeRow row, std::string_view label,
                          std::string_view value) {
                          return _runtime_row_link(row, label, value);
                      };
                parts.push_back(
                    render_runtime_box(_runtime_view(*env), _expanded, link));
            }
        }
        if (narrow) {
            if (parts.empty()) {
                return vbox();
            }
            return panel(scroll_viewport(vbox(std::move(parts)), _scroll)
                       | size(HEIGHT, GREATER_THAN, SIDEBAR_MIN_ROWS))
                | xflex;
        }
        Element body = vbox(std::move(parts));
        return panel(scroll_viewport(std::move(body), _scroll))
            | size(WIDTH, EQUAL, LayoutCtx::LEFT_WIDTH);
    }

    bool OnEvent(Event event) override
    {
        for (const Component& link : _active_links) {
            if (link->OnEvent(event)) {
                return true;
            }
        }
        // Wheel and arrows scroll the column only when it overflows; the
        // chat below keeps the keys otherwise.
        if ((event.is_mouse() || event == Event::ArrowUp
                || event == Event::ArrowDown || event == Event::PageUp
                || event == Event::PageDown || event == Event::Home
                || event == Event::End)
            && _scroll.max_scroll() > 0
            && scroll_viewport_event(_scroll, event)) {
            return true;
        }
        return false;
    }

private:
    RuntimeView _runtime_view(const Environment& env)
    {
        RuntimeView view;
        view.rules       = env.agent_rules_path();
        view.attachments = _attachment_names;
        for (const Skill& skill : *env.sorted_skills()) {
            view.skills.push_back(
                { skill.name, _state->session->is_skill_loaded(skill.path) });
        }
        if (_state->mcp != nullptr) {
            for (const auto& server : _state->mcp->snapshot()) {
                ++view.mcp_total;
                if (server.state == McpServerState::CONNECTED) {
                    view.mcp_connected.push_back({ server.id,
                        std::to_string(server.tool_count) + " tools" });
                }
            }
        }
        const PermissionStore::Snapshot grants
            = _state->permissions->snapshot();
        view.permissions = make_permission_view(_state->runtime_flags,
            _state->permissions->skip_permissions(), *grants);
        return view;
    }

    Component _runtime_row_link(
        RuntimeRow row, std::string_view label, std::string_view value)
    {
        const RuntimeRowText payload { std::string(label), std::string(value) };
        const bool expandable = row != RuntimeRow::PERMISSIONS;
        return _memoized_link(
            _runtime_links, row, payload,
            [this, expandable, row](const RuntimeRowText& current) {
                const bool open = expandable && _expanded.contains(row);
                return hbox({
                    text(expandable ? (open ? "⌄ " : "› ") : "  ")
                        | color(PANEL_FG_DIM),
                    text(current.label) | bold | color(PANEL_FG) | xflex,
                    text(current.value) | color(PANEL_FG) | dim,
                });
            },
            [this, expandable, row](const RuntimeRowText&) {
                if (!expandable) {
                    _open_permissions_modal();
                    return;
                }
                if (!_expanded.erase(row)) {
                    _expanded.insert(row);
                }
                _scroll.scroll_lines(0);
                animation::RequestAnimationFrame();
            },
            [](const RuntimeRowText& next, RuntimeRowText& existing) {
                if (existing != next) {
                    existing = next;
                }
            },
            true);
    }

    void _open_permissions_modal()
    {
        ViewerModal modal { .title = "Permissions",
            .content
            = runtime_permissions_markdown(_runtime_view(*_state->environment)),
            .lang         = "md",
            .line_numbers = false };
        modal.save_name = "Permissions";
        enqueue_user_modal(*_state, std::move(modal));
    }

    template <typename Payload> struct Link {
        std::shared_ptr<Payload> data;
        Component component;
    };

    struct DocumentLink {
        std::string label;
        std::string content;
        bool operator==(const DocumentLink&) const = default;
    };

    struct RuntimeRowText {
        std::string label;
        std::string value;
        bool operator==(const RuntimeRowText&) const = default;
    };

    template <typename Key, typename Payload, typename Render, typename Click,
        typename Update>
    Component _memoized_link(std::map<Key, Link<Payload>>& cache, Key key,
        const Payload& payload, Render render, Click on_click, Update update,
        bool fill = false)
    {
        auto found = cache.find(key);
        if (found == cache.end()) {
            // The closures read the cache-owned payload on every render
            // and click call: per-frame callers never capture a copy, and
            // updated content can never go stale there.
            auto data = std::make_shared<Payload>(payload);
            // Non-focusable: a click must not rotate the app-level Stacked
            // container away from the main column.
            Component component
                = hover_link([data, render] { return render(*data); },
                    [data, click = std::move(on_click)] { click(*data); },
                    PANEL_FG_DIM, fill);
            _links_container->Add(component);
            found = cache
                        .emplace(std::move(key),
                            Link<Payload> { std::move(data), component })
                        .first;
        } else {
            update(payload, *found->second.data);
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
            rows.push_back(_document_link(
                index, labels[index], (*documents)[index].content)
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
            _file_links, file.path, file,
            [](const ChangedFile& current) {
                return changed_file_item(current);
            },
            [this](const ChangedFile& current) {
                _state->review->request_file_jump(changed_file_target(current));
                _navigate(WorkflowPhase::REVIEW);
            },
            [](const ChangedFile& next, ChangedFile& existing) {
                if (existing != next) {
                    existing = next;
                }
            });
    }

    Component _document_link(
        std::size_t index, std::string label, const std::string& content)
    {
        const DocumentLink payload { std::move(label), content };
        return _memoized_link(
            _document_links, index, payload,
            [](const DocumentLink& current) {
                return paragraph(current.label) | color(PANEL_FG);
            },
            [this](const DocumentLink& current) {
                ViewerModal vm { current.label, current.content, "md", 1 };
                vm.line_numbers = false;
                vm.save_name    = current.label;
                enqueue_user_modal(*_state, vm);
            },
            [](const DocumentLink& next, DocumentLink& existing) {
                if (existing != next) {
                    existing = next;
                }
            });
    }

    Component _comment_link(std::size_t id, const std::string& label)
    {
        return _memoized_link(
            _comment_links, id, label,
            [](const std::string& current) { return text(current) | bold; },
            [this, id](
                const std::string&) { _state->review->request_jump(id); },
            [](const std::string& next, std::string& existing) {
                if (existing != next) {
                    existing = next;
                }
            });
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
    std::map<std::size_t, Link<DocumentLink>> _document_links;
    std::map<std::size_t, Link<std::string>> _comment_links;
    std::map<RuntimeRow, Link<RuntimeRowText>> _runtime_links;
    std::vector<Component> _active_links;
    std::set<RuntimeRow> _expanded;
    ScrollView _scroll;
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

namespace {

    std::string rules_basename(const std::optional<std::string>& rules)
    {
        if (!rules) {
            return { };
        }
        return std::filesystem::path(*rules).filename().string();
    }

    std::vector<std::string> loaded_skill_names(
        const std::vector<ContextSkill>& skills)
    {
        std::vector<std::string> names;
        for (const ContextSkill& skill : skills) {
            if (skill.loaded) {
                names.push_back(skill.name);
            }
        }
        return names;
    }

    std::string permission_summary(const PermissionView& view)
    {
        if (view.approvals_skipped) {
            return "skipped";
        }
        const std::size_t count = view.folders.size() + view.commands.size()
            + view.mcp_servers.size() + view.skills.size();
        return std::to_string(count);
    }

    Element runtime_entry(const RuntimeEntry& entry)
    {
        Elements parts { text("  "),
            paragraph(entry.label) | color(PANEL_FG) | dim | xflex };
        if (!entry.detail.empty()) {
            parts.push_back(text(entry.detail) | color(PANEL_FG_DIM));
        }
        return hbox(std::move(parts));
    }

} // namespace

bool has_runtime_content(const RuntimeView& view)
{
    return view.rules.has_value() || !view.attachments.empty()
        || std::any_of(view.skills.begin(), view.skills.end(),
            [](const ContextSkill& skill) { return skill.loaded; })
        || !view.mcp_connected.empty()
        || has_custom_permissions(view.permissions);
}

std::string runtime_permissions_markdown(const RuntimeView& view)
{
    const PermissionView& permissions = view.permissions;
    std::string out;
    out += "| Capability | State |\n| --- | --- |\n";
    out += std::format(
        "| Web | {} |\n", permissions.web_disabled ? "off" : "on");
    out += std::format(
        "| Shell | {} |\n", permissions.shell_disabled ? "off" : "on");
    if (permissions.approvals_skipped) {
        out += "| Approvals | **skipped** |\n";
    }

    const auto append_section = [&out](std::string_view title,
                                    std::string_view column,
                                    const std::vector<std::string>& values) {
        if (values.empty()) {
            return;
        }
        out += std::format("\n### {}\n\n| {} |\n| --- |\n", title, column);
        for (const std::string& value : values) {
            out += std::format("| `{}` |\n", value);
        }
    };
    append_section("Directories", "Path", permissions.folders);
    append_section("Commands", "Command", permissions.commands);
    append_section("MCP servers", "Server", permissions.mcp_servers);
    append_section("Skills", "Skill", permissions.skills);

    if (permissions.folders.empty() && permissions.commands.empty()
        && permissions.mcp_servers.empty() && permissions.skills.empty()) {
        out += "\nNo session grants beyond the workspace.\n";
    }
    return out;
}

ftxui::Element render_runtime_box(const RuntimeView& view,
    const std::set<RuntimeRow>& expanded, const RuntimeRowLink& link)
{
    if (!has_runtime_content(view)) {
        return vbox();
    }

    Elements rows;
    const auto append_row
        = [&rows, &expanded, &link](RuntimeRow row, std::string label,
              std::string value, std::vector<RuntimeEntry> entries) {
              rows.push_back(link(row, label, value)->Render());
              if (!expanded.contains(row)) {
                  return;
              }
              for (const RuntimeEntry& entry : entries) {
                  rows.push_back(runtime_entry(entry));
              }
          };

    if (view.rules || !view.attachments.empty()) {
        const std::string rules_name = rules_basename(view.rules);
        std::vector<RuntimeEntry> files;
        if (view.rules) {
            files.push_back({ *view.rules, { } });
        }
        for (const std::string& attachment : view.attachments) {
            if (!rules_name.empty()
                && std::filesystem::path(attachment).filename() == rules_name) {
                continue;
            }
            files.push_back({ attachment, { } });
        }
        // Read before the move: argument evaluation order is unspecified,
        // so an inline files.size() could observe the moved-from vector.
        const std::string count = std::to_string(files.size());
        append_row(RuntimeRow::FILES, "Files", count, std::move(files));
    }

    const std::vector<std::string> loaded = loaded_skill_names(view.skills);
    if (!loaded.empty()) {
        std::vector<RuntimeEntry> entries;
        for (const std::string& name : loaded) {
            entries.push_back({ name, { } });
        }
        append_row(RuntimeRow::SKILLS, "Skills",
            std::format("{}/{}", loaded.size(), view.skills.size()),
            std::move(entries));
    }

    if (!view.mcp_connected.empty()) {
        append_row(RuntimeRow::MCP, "MCP",
            std::format("{}/{}", view.mcp_connected.size(), view.mcp_total),
            view.mcp_connected);
    }

    if (has_custom_permissions(view.permissions)) {
        append_row(RuntimeRow::PERMISSIONS, "Permissions",
            permission_summary(view.permissions), { });
    }

    Element body = vbox(std::move(rows)) | borderStyled(ROUNDED, PANEL_BORDER);
    return titled_section("Runtime", std::move(body));
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
        if (const auto* skill = std::get_if<SkillGrant>(&grant)) {
            view.skills.push_back(skill->path.string());
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
    sort_unique(view.skills);
    return view;
}

bool has_custom_permissions(const PermissionView& view)
{
    return view.web_disabled || view.shell_disabled || view.approvals_skipped
        || !view.folders.empty() || !view.commands.empty()
        || !view.mcp_servers.empty() || !view.skills.empty();
}

} // namespace imza
