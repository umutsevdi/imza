#include "app/flows.h"
#include "ui/tool_format.h"
#include "ui/ui.h"
#include "workspace/environment.h"

#include <ftxui/component/animation.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <deque>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "common/util.h"

namespace imza {

using namespace ftxui;

namespace {

    enum class ToolPhase { DECIDE, REASON };

    std::string shell_name(const SystemEnvironment& sys)
    {
        if (sys.default_shell.empty()) {
            return "sh";
        }
        return std::filesystem::path(sys.default_shell).filename().string();
    }

    std::string tool_action_description(const PermissionPrompt& req)
    {
        if (!req.description.empty() && req.description != req.name) {
            return req.description;
        }
        if (req.name == "skill") {
            return "Allow this tool to run";
        }
        if (req.name == "shell") {
            return "Run a shell command";
        }
        if (req.name == "edit") {
            return "Replace text in a file";
        }
        if (req.name == "write") {
            return "Write text to a file";
        }
        if (req.name == "insert") {
            return "Insert text into a file";
        }
        if (req.name == "read") {
            return "Read text from a file";
        }
        if (req.name == "list") {
            return "List a directory";
        }
        if (req.name == "load_mcp") {
            return "Connect to an MCP server";
        }
        return "Allow this operation to run";
    }

    std::string preview_text(const std::string& content)
    {
        constexpr std::size_t max_lines      = 8;
        const std::vector<std::string> lines = split_lines(content);
        std::string out;
        const std::size_t shown = std::min(lines.size(), max_lines);
        for (std::size_t i = 0; i < shown; ++i) {
            if (!out.empty()) {
                out += '\n';
            }
            out += lines[i];
        }
        if (lines.size() > shown) {
            out += "\n… " + std::to_string(lines.size() - shown) + " more line"
                + (lines.size() - shown == 1 ? "" : "s");
        }
        return out;
    }

    bool is_markdown_type(std::string_view type)
    {
        const std::string normalized = to_lower(trim(std::string(type)));
        return normalized == "md" || normalized == "markdown";
    }

    int modal_content_width(const ModalPayload& modal)
    {
        const int popup_w
            = std::min(Terminal::Size().dimx - 4, modal_max_width(modal));
        return std::max(40, popup_w - 8);
    }

    Element detail_row(const std::string& label, const std::string& value)
    {
        return hbox({ text(label) | bold | color(PANEL_FG), text("  "),
            paragraph(preview_text(value)) | xflex });
    }

    Element tool_approval_reason(const PermissionPrompt& req)
    {
        std::string message = req.reason;
        if (!message.empty() && !req.target.empty()) {
            message += " · " + req.target;
        } else if (message.empty() && req.name == "shell") {
            message = "May modify files or run external processes";
        } else if (message.empty()
            && (req.name == "edit" || req.name == "write"
                || req.name == "insert")) {
            message = req.target.empty() ? "Will modify a file"
                                         : "Will modify " + req.target;
        } else if (message.empty() && req.name == "read") {
            message = req.target.empty() ? "Will read a file"
                                         : "Will read " + req.target;
        } else if (message.empty() && req.name == "list") {
            message = req.target.empty() ? "Will list a directory"
                                         : "Will list " + req.target;
        } else if (message.empty()) {
            message = "Permission required";
        }
        return text(message) | color(HL_YELLOW);
    }

    Element filesystem_body(const PermissionPrompt& req,
        const FilesystemRequest& request, int width)
    {
        const std::string lang = syntax_type_for_path(req.target);
        return std::visit(
            [&](const auto& operation) -> Element {
                using T = std::decay_t<decltype(operation)>;
                if constexpr (std::is_same_v<T, EditFileRequest>) {
                    return vbox({ section_title("Existing text"),
                        code_block(
                            preview_text(operation.old_text), lang, width),
                        section_title("Replacement"),
                        code_block(
                            preview_text(operation.new_text), lang, width) });
                } else if constexpr (std::is_same_v<T, WriteFileRequest>) {
                    return vbox({ section_title("Content"),
                        code_block(
                            preview_text(operation.text), lang, width) });
                } else if constexpr (std::is_same_v<T, InsertFileRequest>) {
                    const std::string where = operation.line
                        ? "before line " + std::to_string(*operation.line)
                        : "at end of file";
                    return vbox({ section_title("Insert " + where),
                        code_block(
                            preview_text(operation.text), lang, width) });
                } else if constexpr (std::is_same_v<T, ReadFileRequest>) {
                    if (!operation.first_line) {
                        return text("");
                    }
                    std::string range
                        = "lines " + std::to_string(operation.first_line);
                    range += operation.last_line
                        ? "–" + std::to_string(*operation.last_line)
                        : " onward";
                    return text(range) | color(PANEL_FG_DIM);
                } else {
                    return text("");
                }
            },
            request);
    }

    Element tool_request_body(
        const PermissionPrompt& req, const SystemEnvironment& system, int width)
    {
        if (const auto* shell = std::get_if<ShellRequest>(&req.request)) {
            std::string cwd = req.target;
            if (cwd.empty()) {
                std::error_code ec;
                cwd = std::filesystem::current_path(ec).string();
                if (ec) {
                    cwd.clear();
                }
            }
            std::string metadata = (cwd.empty() ? std::string { } : cwd + " · ")
                + "timeout " + std::to_string(shell->timeout.count()) + "s";
            if (!shell->env.empty()) {
                metadata += " · env:";
                std::size_t shown = 0;
                for (const auto& [name, value] : shell->env) {
                    if (shown == 4) {
                        metadata += " …";
                        break;
                    }
                    metadata += (shown == 0 ? " " : ", ") + name;
                    ++shown;
                }
            }
            return vbox({ code_block(preview_text(shell->command),
                              shell_name(system), width),
                hint_bar(metadata) });
        }
        if (const auto* file = std::get_if<FilesystemRequest>(&req.request)) {
            return filesystem_body(req, *file, width);
        }
        if (const auto* skill = std::get_if<SkillRequest>(&req.request)) {
            return vbox({ detail_row("name", skill->name),
                detail_row("path", skill->path),
                detail_row("scope", skill->scope) });
        }
        if (const auto* mcp = std::get_if<McpRequest>(&req.request)) {
            std::string detail = mcp->label != mcp->id && !mcp->label.empty()
                ? mcp->label
                : "";
            if (!mcp->detail.empty()) {
                if (!detail.empty()) {
                    detail += " · ";
                }
                detail += mcp->detail;
            }
            return detail.empty()
                ? detail_row("server", mcp->id)
                : detail_row("server", mcp->id + "  " + detail);
        }
        return text("");
    }

    class ModalView : public ComponentBase {
    public:
        explicit ModalView(std::shared_ptr<ApplicationState> state)
            : _state(std::move(state))
            , _session(_state->session)
        {
        }

        Element OnRender() override
        {
            const Session& st = *_session;
            if (st.exit_confirm()) {
                _ensure_exit_confirm();
                return _exit_confirm_body();
            }
            const std::shared_ptr<const ModalPayload> payload
                = st.modal_handle();
            if (payload->index() == 0) {
                if (_built) {
                    _clear_built_content();
                }
                _built = false;
                return text("");
            }
            _ensure_built(st, *payload);
            if (std::holds_alternative<PermissionPrompt>(*payload)) {
                return _tool_body(std::get<PermissionPrompt>(*payload));
            }
            if (std::holds_alternative<QuestionForm>(*payload)) {
                return _question_body();
            }
            if (std::holds_alternative<ViewerModal>(*payload)) {
                return _viewer_body(std::get<ViewerModal>(*payload));
            }
            return _body->Render();
        }

        // The modal body is rendered directly rather than attached as a
        // child, so animation frames (menu underlines, spinners) must be
        // forwarded by hand or they never advance.
        void OnAnimation(animation::Params& params) override
        {
            if (_body) {
                _body->OnAnimation(params);
            }
        }

        bool OnEvent(Event event) override
        {
            const Session& st = *_session;
            if (st.exit_confirm()) {
                _ensure_exit_confirm();
                if (event == Event::Escape) {
                    _state->session->set_exit_confirm(false);
                    return true;
                }
                return _exit_buttons->OnEvent(event);
            }
            const std::shared_ptr<const ModalPayload> payload
                = st.modal_handle();
            if (payload->index() == 0) {
                return false;
            }
            _ensure_built(st, *payload);
            if (event == Event::Escape) {
                if (std::holds_alternative<ConnectModal>(*payload)) {
                    if (_body && _body->OnEvent(event)) {
                        return true;
                    }
                    imza::close_modal(*_state);
                    return true;
                }
                if (std::holds_alternative<SessionsModal>(*payload) && _body
                    && _body->OnEvent(event)) {
                    return true;
                }
                // The skills view uses Escape to close an open policy list
                // or cancel a removal before the modal itself closes.
                if (std::holds_alternative<SkillsModal>(*payload) && _body
                    && _body->OnEvent(event)) {
                    return true;
                }
                if (std::holds_alternative<PermissionPrompt>(*payload)
                    && _tool_phase == ToolPhase::REASON) {
                    _set_tool_phase(ToolPhase::DECIDE);
                    return true;
                }
                imza::close_modal(*_state);
                return true;
            }
            if (std::holds_alternative<ViewerModal>(*payload)) {
                if (event == Event::Return) {
                    imza::close_modal(*_state);
                    return true;
                }
                if (event == Event::Character("e")) {
                    _export();
                    return true;
                }
                if (event.is_mouse() && _viewer_export
                    && _viewer_export->OnEvent(event)) {
                    return true;
                }
                return _scroll_static(event);
            }
            if (_body) {
                return _body->OnEvent(event);
            }
            return true;
        }

    private:
        struct CardState {
            std::vector<std::string> options;
            std::deque<bool> checked;
            int radio      = 0;
            bool text_live = false;
            std::string free_text;
            int ft_cursor = 0;
            Component selector;
            Component input;
        };

        void _ensure_exit_confirm()
        {
            if (_exit_buttons) {
                return;
            }
            _exit_yes = action_button("Exit", [this] { _state->on_exit(); });
            _exit_no  = action_button(
                "Cancel", [this] { _state->session->set_exit_confirm(false); });
            _exit_buttons = Container::Horizontal({ _exit_yes, _exit_no });
            _exit_yes->TakeFocus();
        }

        Element _exit_confirm_body()
        {
            return vbox({
                       text("Are you sure you want to exit?") | bold,
                       separatorEmpty(),
                       paragraph(
                           "The conversation will be saved. You can continue "
                           "it later with /session.")
                           | color(PANEL_FG_DIM),
                       separatorEmpty(),
                       hbox({ _exit_yes->Render(), text("  "),
                           _exit_no->Render() })
                           | center,
                       separatorEmpty(),
                       hint_bar("Enter exit · Esc cancel"),
                   })
                | xflex;
        }

        void _ensure_built(const Session& st, const ModalPayload& payload)
        {
            if (_built && _serial == st.modal_serial()) {
                return;
            }
            _built  = true;
            _serial = st.modal_serial();
            std::visit(
                [this](const auto& payload) { _build(payload); }, payload);
        }

        void _build(const PermissionPrompt& request)
        {
            build_tool_approval(request);
        }

        void build_tool_approval(const PermissionPrompt& request)
        {
            _tool_phase = ToolPhase::DECIDE;
            _reason_buf.clear();
            _reason_cursor = 0;

            _reason_input = Input(field_option(&_reason_buf, &_reason_cursor,
                "optional reason", { }, [this] { _confirm_reject(); }));

            auto resolve = [this](ToolDecision d, std::string r) {
                imza::resolve_modal(
                    *_state, ModalResult { ToolVerdict { d, std::move(r) } });
            };
            _accept         = action_button("Allow once",
                [resolve] { resolve(ToolDecision::ACCEPT_ONCE, ""); });
            _accept_session = request.allow_for_session
                ? action_button("Allow for this session",
                      [resolve] {
                          resolve(ToolDecision::ACCEPT_FOR_SESSION, "");
                      })
                : Component { };
            _reject         = action_button(
                "Reject", [this] { _set_tool_phase(ToolPhase::REASON); });
            _confirm_reject_button
                = action_button("Reject", [this] { _confirm_reject(); });
            _back = action_button(
                "Back", [this] { _set_tool_phase(ToolPhase::DECIDE); });

            _tool_static = vbox({
                text(tool_action_description(request)) | color(PANEL_FG_DIM),
                tool_approval_reason(request),
                separatorEmpty(),
                tool_request_body(request, *_state->environment->system(),
                    modal_content_width(*_session->modal_handle())),
                separatorEmpty(),
            });
            _build_tool_body();
        }

        void _set_tool_phase(ToolPhase next)
        {
            _tool_phase = next;
            _build_tool_body();
        }

        void _build_tool_body()
        {
            if (_tool_phase == ToolPhase::DECIDE) {
                Components buttons { _accept };
                if (_accept_session) {
                    buttons.push_back(_accept_session);
                }
                buttons.push_back(_reject);
                _body = Container::Horizontal(std::move(buttons));
            } else {
                _body = Container::Vertical({ _reason_input,
                    Container::Horizontal({ _confirm_reject_button, _back }) });
                _reason_input->TakeFocus();
            }
        }

        void _confirm_reject()
        {
            imza::resolve_modal(*_state,
                ModalResult {
                    ToolVerdict { ToolDecision::REJECT, _reason_buf } });
        }

        void _build(const QuestionForm& form)
        {
            _form = form;
            _cards.clear();
            _cards.reserve(form.size());
            _focusables.clear();

            Components children;
            for (const auto& card : form) {
                _cards.emplace_back();
                CardState& cs = _cards.back();
                cs.checked.assign(card.options.size(), false);

                Components rows;
                for (size_t j = 0; j < card.options.size(); ++j) {
                    rows.push_back(
                        _make_option_row(cs, card.options[j], j, card.multi));
                }
                if (card.free_text) {
                    const bool multi    = card.multi;
                    cs.input            = Input(field_option(
                        &cs.free_text, &cs.ft_cursor,
                        card.options.empty() ? "type your answer"
                                             : "type your own answer",
                        [&cs] {
                            if (!trim(cs.free_text).empty()) {
                                cs.text_live = true;
                            }
                        },
                        [this] { _submit_question(); }));
                    Component input_row = Renderer(cs.input, [&cs, multi] {
                        const bool live
                            = cs.text_live && !trim(cs.free_text).empty();
                        return hbox({ text(choice_marker(multi, live)),
                            text(" "), cs.input->Render() });
                    });
                    rows.push_back(input_row);
                }

                cs.selector = Container::Vertical(std::move(rows));
                children.push_back(cs.selector);
                _focusables.push_back(cs.selector);
            }
            _submit = action_button("Submit", [this] { _submit_question(); });
            children.push_back(_submit);
            _focusables.push_back(_submit);

            _body = Container::Vertical(std::move(children));
            if (!_focusables.empty()) {
                _focusables.front()->TakeFocus();
            }
        }

        void _build(const ConnectModal& modal)
        {
            if (modal.entry == ConnectModal::Entry::SUBAGENTS) {
                _body = make_subagents(_state);
            } else {
                _body = make_connect(_state);
            }
        }

        void _build(const SessionsModal&) { _body = make_sessions(_state); }

        void _build(const SkillsModal&) { _body = make_skills(_state); }

        void _build(const McpModal&) { _body = make_mcp(_state); }

        void _build(const PermissionsModal&)
        {
            _body = make_permissions(_state);
        }

        void _build(const ViewerModal& payload)
        {
            _reset_static_scroll();
            if (!_viewer_export) {
                _viewer_export = action_button("Export", [this] { _export(); });
            }
            const int content_width
                = modal_content_width(*_session->modal_handle());
            if (payload.diff.has_value()) {
                _viewer_content = diff_split(*payload.diff, content_width);
            } else if (is_markdown_type(payload.lang)) {
                _viewer_content
                    = render_markdown_element(payload.content, content_width);
            } else if (payload.line_numbers) {
                _viewer_content = code_block_with_lines(payload.content,
                    payload.lang, payload.start_line, content_width);
            } else {
                _viewer_content
                    = code_block(payload.content, payload.lang, content_width);
            }
        }

        void _build(std::monostate) { _clear_built_content(); }

        void _clear_built_content()
        {
            _viewer_content.reset();
            _viewer_export.reset();
            _body.reset();
            _form.clear();
            _cards.clear();
            _focusables.clear();
            _submit.reset();
            _reason_input.reset();
            _accept.reset();
            _accept_session.reset();
            _reject.reset();
            _confirm_reject_button.reset();
            _back.reset();
            _exit_yes.reset();
            _exit_no.reset();
            _exit_buttons.reset();
        }

        Component _make_option_row(
            CardState& cs, const std::string& label, size_t idx, bool multi)
        {
            auto toggle = [&cs, idx, multi] {
                if (multi) {
                    cs.checked[idx] = !cs.checked[idx];
                } else {
                    cs.radio     = static_cast<int>(idx);
                    cs.text_live = false;
                }
            };
            ButtonOption bo;
            bo.transform
                = [&cs, idx, multi, label](const EntryState& s) -> Element {
                const bool selected = multi
                    ? cs.checked[idx]
                    : (!cs.text_live && cs.radio == static_cast<int>(idx));
                return hbox({ text(choice_marker(multi, selected)),
                    choice_label(label, selected, s.focused) });
            };
            bo.on_click   = toggle;
            Component row = Button(label, bo.on_click, bo);
            return space_activates(row, toggle);
        }

        void _submit_question()
        {
            ModalAnswer answer;
            for (size_t i = 0; i < _form.size() && i < _cards.size(); ++i) {
                const QuestionCard& card = _form[i];
                const CardState& cs      = _cards[i];
                QuestionAnswer qa;
                qa.prompt       = card.prompt;
                const bool live = card.free_text && cs.text_live
                    && !trim(cs.free_text).empty();
                if (live) {
                    qa.free_text = cs.free_text;
                } else {
                    if (card.multi) {
                        for (size_t j = 0; j < card.options.size(); ++j) {
                            if (cs.checked[j]) {
                                qa.selected.push_back(card.options[j]);
                            }
                        }
                    } else if (!card.options.empty()
                        && cs.radio < static_cast<int>(card.options.size())) {
                        qa.selected.push_back(
                            card.options[static_cast<size_t>(cs.radio)]);
                    }
                }
                answer.cards.push_back(std::move(qa));
            }
            imza::resolve_modal(*_state, ModalResult { std::move(answer) });
        }

        Element _header_line(std::string_view title)
        {
            const size_t remaining = _state->session->queue.size();
            return hbox({
                text(std::string(title)) | bold,
                filler(),
                text(std::to_string(remaining) + " remaining") | dim
                    | color(PANEL_FG_DIM),
            });
        }

        Element _tool_body(const PermissionPrompt& req)
        {
            Elements rows { _header_line(tool_display_name(req.name)),
                _tool_static };
            if (_tool_phase == ToolPhase::REASON) {
                rows.push_back(section_title("Reason for rejecting", PANEL_FG));
                rows.push_back(_reason_input->Render());
                rows.push_back(hbox({
                    _confirm_reject_button->Render(),
                    text(" "),
                    _back->Render(),
                }));
                rows.push_back(separatorEmpty());
                rows.push_back(hint_bar("Enter confirm rejection · Esc back"));
            } else {
                rows.push_back(hbox({
                    _accept->Render(),
                    _accept_session
                        ? hbox({ text(" "), _accept_session->Render(),
                              text(" ") })
                        : text(" "),
                    _reject->Render(),
                }));
                rows.push_back(separatorEmpty());
                if (_accept_session) {
                    rows.push_back(hint_bar(
                        "allow lasts until directory or session changes"));
                }
                rows.push_back(hint_bar("Esc reject"));
            }
            return vbox(std::move(rows)) | xflex;
        }

        Element _question_body()
        {
            Elements rows { _header_line("Question") };
            for (size_t i = 0; i < _form.size(); ++i) {
                rows.push_back(separatorEmpty());
                rows.push_back(text(_form[i].prompt) | bold);
                if (_cards[i].selector) {
                    rows.push_back(_cards[i].selector->Render());
                }
            }
            rows.push_back(separatorEmpty());
            rows.push_back(_submit->Render() | center);
            rows.push_back(separatorEmpty());
            rows.push_back(hint_bar("↑/↓ navigate · Enter confirm"));
            return vbox(std::move(rows)) | xflex;
        }

        void _export()
        {
            const std::shared_ptr<const ModalPayload> handle
                = _session->modal_handle();
            const ViewerModal* payload = std::get_if<ViewerModal>(handle.get());
            if (payload == nullptr) {
                return;
            }
            std::string error;
            const std::string path
                = export_viewer_content(*_state, *payload, error);
            if (!_state->notifications) {
                return;
            }
            if (!error.empty()) {
                _state->notifications->publish(true, std::move(error));
                return;
            }
            _state->notifications->publish(false, "Exported to " + path);
        }

        Element _viewer_body(const ViewerModal& payload)
        {
            Elements rows { _header_line(payload.title), separatorEmpty(),
                _static_viewport(_viewer_content, payload.metadata,
                    _viewer_export->Render()) };
            return vbox(std::move(rows)) | xflex;
        }

        Element _static_viewport(
            Element content, const std::string& metadata, Element trailing)
        {
            Element viewport
                = scroll_viewport(std::move(content), _static_view);
            Elements rows { std::move(viewport), separatorEmpty() };
            if (!metadata.empty()) {
                rows.push_back(hint_bar(metadata));
            }
            rows.push_back(hbox({ std::move(trailing), filler(),
                text("↑/↓ scroll · e export · Esc close") | dim }));
            return vbox(std::move(rows)) | xflex | yflex;
        }

        bool _scroll_static(Event event)
        {
            return scroll_viewport_event(_static_view, event);
        }

        void _reset_static_scroll() { _static_view = { }; }

        std::shared_ptr<ApplicationState> _state;
        std::shared_ptr<Session> _session;
        bool _built           = false;
        std::uint64_t _serial = 0;

        QuestionForm _form;
        std::vector<CardState> _cards;
        std::vector<Component> _focusables;
        Component _submit;

        ToolPhase _tool_phase = ToolPhase::DECIDE;
        std::string _reason_buf;
        int _reason_cursor = 0;
        ScrollView _static_view { };
        Element _tool_static;
        Component _reason_input;
        Component _accept;
        Component _accept_session;
        Component _reject;
        Component _confirm_reject_button;
        Component _back;
        Element _viewer_content;

        Component _viewer_export;

        Component _exit_yes;
        Component _exit_no;
        Component _exit_buttons;
        Component _body;
    };

} // namespace

int modal_max_width(const ModalPayload& modal)
{
    return std::holds_alternative<ViewerModal>(modal)
            && std::get<ViewerModal>(modal).diff.has_value()
        ? DIFF_VIEWER_MODAL_MAX_WIDTH
        : MODAL_MAX_WIDTH;
}

ftxui::Component make_modal(std::shared_ptr<ApplicationState> state)
{
    return ftxui::Make<ModalView>(std::move(state));
}

} // namespace imza
