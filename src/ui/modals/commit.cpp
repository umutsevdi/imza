#include "app/application_state.h"
#include "app/flows.h"
#include "common/modal.h"
#include "common/util.h"
#include "ui/ui.h"
#include "workspace/environment.h"

#include <ftxui/component/animation.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <atomic>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace imza {

using namespace ftxui;

namespace {

    constexpr int BODY_MIN_HEIGHT = 2;
    constexpr int BODY_MAX_HEIGHT = 8;

    // The first message line is the title: brighter and bold, like the
    // subject line git and forges show.
    InputOption title_option(
        std::string* content, int* cursor, std::function<void()> on_change)
    {
        InputOption option = field_option(
            content, cursor, "Commit title", std::move(on_change));
        option.transform = [](InputState state) {
            if (state.is_placeholder) {
                state.element |= dim | color(PANEL_FG_DIM);
            } else {
                state.element |= bold | color(PANEL_FG);
            }
            state.element
                |= bgcolor(state.focused ? PANEL_COLOR_FOCUS : PANEL_COLOR);
            return state.element;
        };
        return option;
    }

    InputOption body_option(
        std::string* content, int* cursor, std::function<void()> on_change)
    {
        return multiline_field_option(
            content, cursor, "Description (optional)", std::move(on_change));
    }

    // "6 files changed  +61 −1 will be committed to:", then the branch and
    // remote on their own line.
    Elements commit_summary(const StagedStat& stat,
        const RepositoryState& repository,
        const std::optional<std::string>& origin)
    {
        Elements rows;
        const std::string count = std::format(
            "{} file{} changed", stat.files, stat.files == 1 ? "" : "s");
        rows.push_back(hbox({ text(count), text("  "),
                           diffstat_chip(stat.additions, stat.deletions),
                           text(" will be committed to:"), filler() })
            | color(PANEL_FG_DIM));
        Elements target;
        target.push_back(text(repository.branch) | color(HL_GREEN));
        if (origin.has_value()) {
            Element url = text("  " + *origin) | color(HL_CYAN);
            // Only web URLs open in a browser; ssh and git:// stay text.
            if (origin->starts_with("http://")
                || origin->starts_with("https://")) {
                url = std::move(url) | hyperlink(*origin) | underlined;
            }
            target.push_back(std::move(url));
        }
        rows.push_back(hbox(std::move(target)));
        return rows;
    }

    // Resolved off the UI thread and shared with the completion, so the
    // view may close before the answer lands.
    struct ContextState {
        bool known = false;
        Environment::CommitContext context;
    };

    void load_commit_context(const std::shared_ptr<ApplicationState>& state,
        const std::shared_ptr<ContextState>& context)
    {
        state->environment->commit_context(
            state->post, [context](Environment::CommitContext value) {
                context->context = std::move(value);
                context->known   = true;
            });
    }

    class CommitView : public ComponentBase {
    public:
        explicit CommitView(std::shared_ptr<ApplicationState> state)
            : _state(std::move(state))
        {
            // Runs on the git worker: touches only shared holders, never
            // the view, so a publish racing the dialog's close is safe.
            _repository_subscription
                = _state->environment->subscribe_to_repository_change(
                    [state = _state, context = _context] {
                        animation::RequestAnimationFrame();
                        load_commit_context(state, context);
                    });
            _title_input = Input(title_option(
                &_title, &_title_cursor, [this] { _error.clear(); }));
            _body_input  = Input(
                body_option(&_body, &_body_cursor, [this] { _error.clear(); }));
            _commit_button
                = action_button(&_commit_label, [this] { _commit(false); });
            _push_button = action_button("Push", [this] { _commit(true); });
            Add(Container::Vertical({ _title_input, _body_input,
                Container::Horizontal({ _commit_button, _push_button }) }));
            _title_input->TakeFocus();
            load_commit_context(_state, _context);
        }

        Element OnRender() override
        {
            const auto repository = _state->environment->repository();
            _commit_label = *_committed ? "Committed" : "Commit";
            Elements rows = modal_header("Commit");
            rows.push_back(separatorEmpty());
            rows.push_back(_title_input->Render());
            rows.push_back(_body_input->Render()
                | size(HEIGHT, GREATER_THAN, BODY_MIN_HEIGHT)
                | size(HEIGHT, LESS_THAN, BODY_MAX_HEIGHT + 1));
            rows.push_back(separatorEmpty());
            if (!_error.empty()) {
                rows.push_back(paragraph(_error) | color(HL_RED));
                rows.push_back(separatorEmpty());
            }
            if (*_committed) {
                rows.push_back(text("Committed.") | color(HL_GREEN));
            } else if (repository != nullptr) {
                const std::optional<std::string> origin
                    = _context->known ? _context->context.origin : std::nullopt;
                for (Element& row : commit_summary(
                         _context->context.staged, *repository, origin)) {
                    rows.push_back(std::move(row));
                }
            }
            rows.push_back(separatorEmpty());
            Elements actions = { _commit_button->Render(), text("  "),
                _push_button->Render(), filler() };
            if (*_committed) {
                actions.push_back(text("Esc close") | color(PANEL_FG_DIM));
            } else {
                actions.push_back(
                    text("Enter commit · ") | color(PANEL_FG_DIM));
                actions.push_back(
                    text(std::string(MULTILINE_KEY) + " new line · Esc close")
                    | color(PANEL_FG_DIM));
            }
            rows.push_back(hbox(std::move(actions)));
            return vbox(std::move(rows)) | xflex;
        }

        bool OnEvent(Event event) override
        {
            if (event == Event::Escape) {
                imza::close_modal(*_state);
                return true;
            }
            // Enter commits from either field. The body is multiline, so
            // a bare Return would otherwise insert a newline there;
            // MULTILINE_KEY is what adds one.
            if (is_alt_enter(event)) {
                if (_body_input->Focused()) {
                    insert_newline_at(_body, _body_cursor);
                }
                return true;
            }
            // A focused button keeps Enter for itself; from the fields,
            // Enter commits.
            if (event == Event::Return
                && (_title_input->Focused() || _body_input->Focused())) {
                _commit(false);
                return true;
            }
            // Between the fields: down from the title, up from the body's
            // first line. The multiline body consumes Up/Down itself for
            // cursor movement, so only the edge cases are intercepted.
            if (event == Event::ArrowDown && _title_input->Focused()) {
                _body_input->TakeFocus();
                return true;
            }
            const std::size_t first_newline = _body.find('\n');
            if (event == Event::ArrowUp && _body_input->Focused()
                && (first_newline == std::string::npos
                    || _body_cursor <= static_cast<int>(first_newline))) {
                _title_input->TakeFocus();
                return true;
            }
            return ComponentBase::OnEvent(event);
        }

    private:
        std::string _message() const
        {
            if (trim(_body).empty()) {
                return _title;
            }
            return _title + "\n\n" + _body;
        }

        void _commit(bool push)
        {
            if (*_committed) {
                // The commit landed: Push publishes it and closes the
                // dialog, Commit has nothing left to take.
                if (push) {
                    imza::push_and_notify(*_state);
                    imza::close_modal(*_state);
                }
                return;
            }
            if (trim(_title).empty()) {
                _error = "Write a commit title first.";
                _title_input->TakeFocus();
                return;
            }
            if (push) {
                imza::resolve_modal(
                    *_state, ModalResult { CommitResult { _message(), true } });
                return;
            }
            // Commit-only keeps the dialog open so Push stays reachable.
            // The flag is shared because the completion may outlive the
            // dialog.
            imza::commit_and_notify(
                *_state, _message(), false, [committed = _committed](bool ok) {
                    committed->store(ok);
                    animation::RequestAnimationFrame();
                });
        }

        std::shared_ptr<ApplicationState> _state;
        Signal<>::Subscription _repository_subscription;
        Component _title_input;
        Component _body_input;
        Component _commit_button;
        Component _push_button;
        std::string _commit_label = "Commit";
        std::shared_ptr<std::atomic<bool>> _committed
            = std::make_shared<std::atomic<bool>>(false);
        std::string _title;
        int _title_cursor = 0;
        std::string _body;
        int _body_cursor = 0;
        std::string _error;
        std::shared_ptr<ContextState> _context
            = std::make_shared<ContextState>();
    };

} // namespace

ftxui::Component make_commit(std::shared_ptr<ApplicationState> state)
{
    return ftxui::Make<CommitView>(std::move(state));
}

} // namespace imza
