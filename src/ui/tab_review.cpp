#include "app/flows.h"
#include "common/types.h"
#include "turn/delegation.h"
#include "turn/prompt_store.h"
#include "ui/annotations.h"
#include "ui/ui.h"
#include "workspace/review_subsystem.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <format>
#include <set>
#include <thread>

namespace imza {

ftxui::Element review_line_background(ftxui::Element row,
    std::optional<ftxui::Color> change_background, bool selected)
{
    if (selected) {
        return std::move(row) | ftxui::bgcolor(PANEL_COLOR_FOCUS);
    }
    if (change_background) {
        return std::move(row) | ftxui::bgcolor(*change_background);
    }
    return row;
}

namespace {

    using namespace ftxui;

    std::string line_range(std::size_t start, std::size_t count)
    {
        if (count <= 1) {
            return std::to_string(start);
        }
        return std::format("{}–{}", start, start + count - 1);
    }

    std::string hunk_label(const ReviewHunk& hunk)
    {
        const std::string old_range = hunk.old_count == 0
            ? "∅"
            : line_range(hunk.old_start, hunk.old_count);
        const std::string new_range = hunk.new_count == 0
            ? "∅"
            : line_range(hunk.new_start, hunk.new_count);
        return old_range + " → " + new_range;
    }

    // The gutter marker and background for one review line: additions and
    // deletions tint, everything else stays plain.
    struct LineChangeStyle {
        std::string marker = " ";
        std::optional<Color> background;
    };

    LineChangeStyle line_change_style(ReviewLine::Kind kind)
    {
        if (kind == ReviewLine::Kind::ADDITION) {
            return { diff_marker(true), diff_background(true) };
        }
        if (kind == ReviewLine::Kind::DELETION) {
            return { diff_marker(false), diff_background(false) };
        }
        return { };
    }

    class Review : public ComponentBase {
    public:
        Review(std::shared_ptr<ApplicationState> state, LayoutFn layout,
            WorkflowNavigateFn navigate)
            : _state(std::move(state))
            , _layout(std::move(layout))
            , _navigate(std::move(navigate))
            , _editor([this] { animation::RequestAnimationFrame(); })
            , _repository_subscription(
                  _state->environment->subscribe_to_repository_change(
                      [this] { _request_reload(); }))
            , _workspace_subscription(
                  _state->environment->subscribe_to_workspace_change(
                      [this] { _request_reload(); }))
            , _review_subscription(_state->review->subscribe_to_change(
                  [] { animation::RequestAnimationFrame(); }))
        {
            _plan_button
                = action_button("Send to Plan", [this] { _send_to_plan(); });
            // Each pass owns one slot: the action button when idle, Cancel
            // in place while it runs, so the row never grows or reorders.
            _review_action_button
                = action_button("AI Review", [this] { _provide_review(); });
            _review_cancel_button
                = action_button("Cancel", [this] { _cancel_review(); });
            _handoff_action_button = action_button(
                "Handoff", [this] { imza::run_handoff(*_state); });
            _handoff_cancel_button
                = action_button("Cancel", [this] { _cancel_handoff(); });
            Add(_editor.input());
            Add(_plan_button);
            Add(_review_action_button);
            Add(_review_cancel_button);
            Add(_handoff_action_button);
            Add(_handoff_cancel_button);
        }

        Element OnRender() override
        {
            if (!_load_requested) {
                _load_requested = true;
                _request_reload();
            }
            const ReviewSubsystem::Snapshot snapshot
                = _state->review->snapshot();
            _consume_jump(snapshot);
            _visible.clear();
            _boxes.clear();
            _box_rows.clear();
            _rendered_y     = 0;
            _skipped_height = 0;

            if (snapshot.status == ReviewSubsystem::LoadStatus::IDLE
                || snapshot.status == ReviewSubsystem::LoadStatus::LOADING) {
                return center(text("Loading changes…") | color(PANEL_FG_DIM))
                    | flex;
            }
            if (snapshot.status == ReviewSubsystem::LoadStatus::ERROR) {
                return center(vbox({ text("Unable to load changes") | bold,
                           paragraph(snapshot.error) | color(PANEL_FG_DIM) }))
                    | flex;
            }
            _rendered_review = snapshot.review;
            if (snapshot.review->files.empty()) {
                return center(
                           text("Working tree is clean") | color(PANEL_FG_DIM))
                    | flex;
            }
            const LayoutCtx ctx     = _layout();
            const int review_width  = review_content_width(ctx);
            const bool side_by_side = review_width >= 100;
            _render_radius = ctx.height > 0 ? std::max(20, ctx.height) : 120;
            _prepare_highlights(*snapshot.review, review_width, side_by_side);

            Elements rows;
            for (std::size_t file_index = 0;
                file_index < snapshot.review->files.size(); ++file_index) {
                const ReviewFile& file = snapshot.review->files[file_index];
                _push_file(rows, snapshot, file, file_index, review_width,
                    side_by_side);
                if (file_index + 1 < snapshot.review->files.size()) {
                    _flush_spacer(rows);
                    rows.push_back(separatorEmpty());
                    ++_rendered_y;
                }
            }
            if (_pending_jump) {
                for (std::size_t i = 0; i < _visible.size(); ++i) {
                    if (_visible[i].comment_id == _pending_jump) {
                        _selected         = static_cast<int>(i);
                        _selected_comment = _pending_jump;
                        _pending_jump.reset();
                        animation::RequestAnimationFrame();
                        break;
                    }
                }
            }
            if (_pending_file_jump) {
                for (std::size_t i = 0; i < _visible.size(); ++i) {
                    if (_visible[i].kind == VisibleRow::Kind::FILE
                        && _path(_visible[i].file_index)
                            == *_pending_file_jump) {
                        _selected = static_cast<int>(i);
                        _selected_comment.reset();
                        animation::RequestAnimationFrame();
                        break;
                    }
                }
                _pending_file_jump.reset();
            }
            if (_visible.empty()) {
                return text("") | flex;
            }
            _selected = std::clamp(
                _selected, 0, static_cast<int>(_visible.size()) - 1);

            const int selected_y = _visible[_selected].display_y;
            Element content      = vbox(std::move(rows))
                | focusPosition(0, selected_y) | yframe | vscroll_indicator
                | flex;
            static constexpr std::string_view top_hint
                = "p Send to Plan · r AI Review · h Handoff · Ctrl+S Sidechat";
            const std::string hint_line = _editor.is_open() ? "Enter save · "
                    + std::string(MULTILINE_KEY) + " new line · Esc cancel"
                : _selected_comment ? "↑↓ navigate · e edit · DEL delete"
                                    : "↑↓ navigate · [] files · Enter comment "
                                      "· c collapse";
            const ReviewSubsystem::Snapshot review_snapshot
                = _state->review->snapshot();
            Elements bottom { };
            const bool review_running
                = review_snapshot.findings.task_id.has_value();
            const bool handoff_running
                = review_snapshot.handoff.task_id.has_value();
            // One slot per pass, in fixed order; a running pass swaps its
            // action button for Cancel in place.
            _plan_box         = Box { };
            _review_slot_box  = Box { };
            _handoff_slot_box = Box { };
            Elements actions {
                _plan_button->Render() | reflect(_plan_box),
                text(" "),
                (review_running ? _review_cancel_button->Render()
                                : _review_action_button->Render())
                    | reflect(_review_slot_box),
                text(" "),
                (handoff_running ? _handoff_cancel_button->Render()
                                 : _handoff_action_button->Render())
                    | reflect(_handoff_slot_box),
            };
            actions.push_back(filler());
            actions.push_back(text(hint_line) | dim);
            bottom.push_back(hbox({ filler(), text(top_hint) | dim }));
            bottom.push_back(hbox(std::move(actions)));
            return vbox({ std::move(content), vbox(std::move(bottom)) }) | flex;
        }

        bool OnEvent(Event event) override
        {
            if (_editor.is_open()) {
                switch (_editor.handle_event(event)) {
                case AnnotationAction::CANCEL: _close_editor(); return true;
                case AnnotationAction::SAVE: _save_editor(); return true;
                case AnnotationAction::HANDLED: return true;
                case AnnotationAction::NONE: break;
                }
                return _editor.input()->OnEvent(event);
            }
            if (event.is_mouse()) {
                const Mouse& mouse = event.mouse();
                if (mouse.button == Mouse::WheelUp) {
                    return _move(-SCROLL_WHEEL_STEP);
                }
                if (mouse.button == Mouse::WheelDown) {
                    return _move(SCROLL_WHEEL_STEP);
                }
                if (mouse.button == Mouse::Left
                    && mouse.motion == Mouse::Pressed) {
                    if (_button_press(mouse)) {
                        return true;
                    }
                    for (std::size_t i = 0; i < _boxes.size(); ++i) {
                        if (_boxes[i].Contain(mouse.x, mouse.y)) {
                            _selected = _box_rows[i];
                            _activate(true);
                            return true;
                        }
                    }
                }
                return false;
            }
            if (event == Event::ArrowUp) {
                return _move(-1);
            }
            if (event == Event::ArrowDown) {
                return _move(1);
            }
            if (event == Event::PageUp) {
                return _move(-10);
            }
            if (event == Event::PageDown) {
                return _move(10);
            }
            if (event == Event::Home) {
                _selected = 0;
                return true;
            }
            if (event == Event::End) {
                _selected = std::max(0, static_cast<int>(_visible.size()) - 1);
                return true;
            }
            if (event == Event::Character("[")) {
                return _jump_file(-1);
            }
            if (event == Event::Character("]")) {
                return _jump_file(1);
            }
            if (event == Event::Return) {
                return _activate(false, true);
            }
            if (event == Event::Character(" ")) {
                return _activate(false);
            }
            if (event == Event::Character("c")) {
                return _collapse_selected();
            }
            if (event == Event::Character("e")) {
                return _edit_comment();
            }
            if (event == Event::Delete) {
                return _delete_comment();
            }
            if (event == Event::Character("p")) {
                _send_to_plan();
                return true;
            }
            if (event == Event::Character("r")) {
                _provide_review();
                return true;
            }
            if (event == Event::Character("h")) {
                imza::run_handoff(*_state);
                return true;
            }
            return false;
        }

    private:
        void _send_to_plan()
        {
            if (_state->review->snapshot().findings.task_id) {
                return;
            }
            const std::vector<ReviewComment> comments
                = _state->review->comments();
            if (comments.empty()) {
                _state->session->set_error(
                    "Add a review comment before sending.");
                return;
            }
            if (!_state->providers->active_selection()) {
                _state->session->set_error(NO_MODEL_SELECTED);
                return;
            }
            std::string prompt = format_review_plan_prompt(
                _state->prompts->review_plan(), comments);
            _navigate(WorkflowPhase::PLAN);
            imza::submit(*_state, std::move(prompt));
            _state->review->clear_comments();
            _selected_comment.reset();
            _pending_jump.reset();
        }

        void _provide_review()
        {
            const ReviewSubsystem::Snapshot snapshot
                = _state->review->snapshot();
            if (snapshot.status != ReviewSubsystem::LoadStatus::LOADED
                || snapshot.review->files.empty()) {
                _state->session->set_error("There are no changes to review.");
                return;
            }
            std::string prompt = format_ai_review_prompt(
                _state->prompts->review(), *snapshot.review, snapshot.comments);
            std::string error;
            (void)run_review_pass(*_state,
                ReviewPassRequest { .pass = ReviewPass::FINDINGS,
                    .prompt               = std::move(prompt),
                    .label                = "AI review",
                    .complete =
                        [state = _state](std::string output) {
                            const ReviewSubsystem::Snapshot current
                                = state->review->snapshot();
                            if (!current.review) {
                                return std::string { };
                            }
                            AiReviewParseResult parsed
                                = parse_ai_review_response(
                                    output, *current.review);
                            if (auto* parse_error
                                = std::get_if<std::string>(&parsed)) {
                                return std::move(*parse_error);
                            }
                            std::vector<ReviewCommentDraft> drafts = std::move(
                                std::get<std::vector<ReviewCommentDraft>>(
                                    parsed));
                            const std::size_t count = drafts.size();
                            state->review->add_comments(std::move(drafts));
                            if (state->notifications) {
                                state->notifications->publish(false,
                                    count == 1 ? "AI review found 1 finding."
                                               : "AI review found "
                                            + std::to_string(count)
                                            + " findings.");
                            }
                            return std::string { };
                        } },
                error);
            if (!error.empty()) {
                _state->session->set_error(std::move(error));
            }
        }

        void _cancel_review()
        {
            (void)cancel_review_pass(*_state, ReviewPass::FINDINGS);
        }

        void _cancel_handoff()
        {
            (void)cancel_review_pass(*_state, ReviewPass::HANDOFF);
        }

        struct VisibleRow {
            enum class Kind { FILE, HUNK, LINE, COMMENT };
            Kind kind              = Kind::HUNK;
            std::size_t file_index = 0;
            const ReviewLine* line = nullptr;
            std::optional<std::size_t> comment_id;
            int display_y = 0;
        };

        void _prepare_highlights(
            const RepositoryReview& review, int review_width, bool side_by_side)
        {
            if (_highlighted_review == &review
                && _highlighted_width == review_width
                && _highlighted_side_by_side == side_by_side) {
                return;
            }
            _highlights.clear();
            _highlighted_review       = &review;
            _highlighted_width        = review_width;
            _highlighted_side_by_side = side_by_side;
        }

        // Highlighted visual rows for a non-META line, falling back to the
        // pre-wrapped plain segments while highlights are loading.
        Elements _highlighted_rows(const ReviewLine& line, bool old_side,
            const std::vector<std::string>& fallback) const
        {
            const auto found = _highlights.find(&line);
            if (found != _highlights.end()) {
                const std::vector<Element>& highlighted = old_side
                    ? found->second.old_side
                    : found->second.new_side;
                if (!highlighted.empty()) {
                    return highlighted;
                }
            }
            Elements rows;
            rows.reserve(fallback.size());
            for (const std::string& segment : fallback) {
                rows.push_back(text(segment) | color(PANEL_FG));
            }
            return rows;
        }

        void _flush_spacer(Elements& rows)
        {
            if (_skipped_height == 0) {
                return;
            }
            rows.push_back(vertical_space(_skipped_height));
            _skipped_height = 0;
        }

        template <typename Render>
        void _push(Elements& rows, Render&& render, VisibleRow visible,
            bool selected, int height = 1)
        {
            const int row_index = static_cast<int>(_visible.size());
            visible.display_y   = _rendered_y;
            _rendered_y += height;
            _visible.push_back(std::move(visible));
            if (std::abs(row_index - _selected) > _render_radius) {
                _skipped_height += height;
                return;
            }
            _flush_spacer(rows);
            Element row = render();
            if (selected) {
                row = std::move(row) | bgcolor(PANEL_COLOR_FOCUS);
            }
            _boxes.push_back(Box { });
            _box_rows.push_back(row_index);
            rows.push_back(std::move(row) | xflex | reflect(_boxes.back()));
        }

        void _push_file(Elements& rows,
            const ReviewSubsystem::Snapshot& snapshot, const ReviewFile& file,
            std::size_t file_index, int review_width, bool side_by_side)
        {
            Elements file_rows;
            const std::string& path
                = file.new_path.empty() ? file.old_path : file.new_path;
            const bool collapsed       = _collapsed.contains(path);
            const std::size_t comments = std::ranges::count_if(
                snapshot.comments, [&path](const ReviewComment& comment) {
                    return comment.anchor.file == path;
                });
            Elements header_parts {
                text(collapsed ? "› " : "⌄ ") | color(PANEL_FG_DIM),
                text(path) | bold | color(PANEL_FG),
                filler(),
                diffstat_chip(file.additions, file.deletions),
            };
            if (comments > 0) {
                header_parts.push_back(text(std::format("  💬 {}", comments))
                    | color(PANEL_FG_DIM));
            }
            Element header = hbox(std::move(header_parts));
            _push(
                file_rows,
                [header = std::move(header)]() mutable {
                    return std::move(header);
                },
                VisibleRow { VisibleRow::Kind::FILE, file_index, nullptr,
                    std::nullopt, 0 },
                _selected == static_cast<int>(_visible.size()));
            if (!collapsed && file.kind == ReviewFile::Kind::BINARY) {
                _push(
                    file_rows,
                    [] {
                        return text("  Binary file changed")
                            | color(PANEL_FG_DIM);
                    },
                    VisibleRow { VisibleRow::Kind::HUNK, file_index, nullptr,
                        std::nullopt, 0 },
                    _selected == static_cast<int>(_visible.size()));
            } else if (!collapsed) {
                const int side_width = diff_side_width(review_width);
                for (const ReviewHunk& hunk : file.hunks) {
                    const int hunk_begin = static_cast<int>(_visible.size());
                    const int hunk_end   = hunk_begin + 1
                        + static_cast<int>(hunk.lines.size())
                        + static_cast<int>(snapshot.comments.size());
                    if (hunk_begin <= _selected + _render_radius
                        && hunk_end >= _selected - _render_radius) {
                        append_review_hunk_highlights(_highlights, hunk, path,
                            review_width, side_by_side);
                    }
                    _push(
                        file_rows,
                        [&hunk] {
                            return hbox({ text("  "),
                                text(hunk_label(hunk)) | bold
                                    | color(PANEL_FG_DIM),
                                filler() });
                        },
                        VisibleRow { VisibleRow::Kind::HUNK, file_index,
                            nullptr, std::nullopt, 0 },
                        _selected == static_cast<int>(_visible.size()));
                    if (side_by_side) {
                        _push_side_by_side_hunk(file_rows, snapshot, file_index,
                            path, hunk, side_width);
                    } else {
                        for (const ReviewLine& line : hunk.lines) {
                            _push_unified_line(file_rows, snapshot, file_index,
                                path, line, review_width);
                        }
                    }
                }
            }
            _flush_spacer(file_rows);
            rows.push_back(panel(vbox(std::move(file_rows))));
        }

        static ReviewLineAnchor _anchor(
            const std::string& path, const ReviewLine& line)
        {
            return { path, line.old_line, line.new_line, line.content };
        }

        static bool _matches(const ReviewLineAnchor& anchor,
            const std::string& path, const ReviewLine& line)
        {
            return anchor.file == path && anchor.old_line == line.old_line
                && anchor.new_line == line.new_line
                && anchor.content == line.content;
        }

        void _push_comments(Elements& rows,
            const ReviewSubsystem::Snapshot& snapshot, std::size_t file_index,
            const std::string& path, const ReviewLine& line, int review_width)
        {
            for (const ReviewComment& comment : snapshot.comments) {
                if (!_matches(comment.anchor, path, line)) {
                    continue;
                }
                const int content_width = std::max(1, review_width - 15);
                int height              = 0;
                Element card            = annotation_note_card(
                    comment.body, content_width, &height);
                _push(
                    rows, [card = std::move(card)] { return card; },
                    VisibleRow { VisibleRow::Kind::COMMENT, file_index, nullptr,
                        comment.id, 0 },
                    _selected == static_cast<int>(_visible.size()), height);
            }
        }

        void _push_editor(Elements& rows, const std::string& path,
            const ReviewLine& line, int review_width)
        {
            if (!_editor.is_open() || !_matches(_editor.anchor(), path, line)) {
                return;
            }
            const int content_width = std::max(1, review_width - 15);
            int height              = 0;
            _flush_spacer(rows);
            rows.push_back(annotation_editor_card(
                _editor.draft(), content_width, "Leave a comment", &height));
            _rendered_y += height;
        }

        void _push_unified_line(Elements& rows,
            const ReviewSubsystem::Snapshot& snapshot, std::size_t file_index,
            const std::string& path, const ReviewLine& line, int review_width)
        {
            const LineChangeStyle style     = line_change_style(line.kind);
            std::string marker              = style.marker;
            std::optional<Color> background = style.background;
            const int content_width         = diff_content_width(review_width);
            const std::vector<std::string> segments
                = wrap_text(line.content, content_width);
            const bool selected
                = _selected == static_cast<int>(_visible.size());
            _push(
                rows,
                [this, &line, marker = std::move(marker), background, segments,
                    selected] {
                    const bool old_side
                        = line.kind == ReviewLine::Kind::DELETION;
                    Elements content_rows;
                    if (line.kind == ReviewLine::Kind::META) {
                        for (const std::string& segment : segments) {
                            content_rows.push_back(
                                text(segment) | color(PANEL_FG_DIM));
                        }
                    } else {
                        const Elements highlighted
                            = _highlighted_rows(line, old_side, segments);
                        for (std::size_t i = 0; i < segments.size(); ++i) {
                            content_rows.push_back(i < highlighted.size()
                                    ? highlighted[i]
                                    : text(segments[i]) | color(PANEL_FG));
                        }
                    }
                    return review_line_background(
                        vbox(diff_line_rows(5, true, line.old_line,
                            line.new_line, std::move(marker),
                            std::move(content_rows))),
                        background, selected);
                },
                VisibleRow { VisibleRow::Kind::LINE, file_index, &line,
                    std::nullopt, 0 },
                selected, static_cast<int>(segments.size()));
            _push_comments(
                rows, snapshot, file_index, path, line, review_width);
            _push_editor(rows, path, line, review_width);
        }

        Element _side_line(const ReviewLine* line, bool old_side,
            int side_width, int height, bool selected) const
        {
            const std::optional<std::size_t> number = line == nullptr
                ? std::nullopt
                : old_side ? line->old_line
                           : line->new_line;
            const std::string number_text
                = number ? std::format("{:>5}", *number) : std::string(5, ' ');
            if (line == nullptr) {
                return hbox({ text(number_text), text("  "), filler() })
                    | size(WIDTH, EQUAL, side_width);
            }
            const LineChangeStyle style     = line_change_style(line->kind);
            std::string marker              = style.marker;
            std::optional<Color> background = style.background;
            const std::vector<std::string> segments = wrap_text(
                line->content, review_side_content_width(side_width));
            const Elements highlighted
                = _highlighted_rows(*line, old_side, segments);
            Elements content_rows;
            content_rows.reserve(segments.size());
            for (std::size_t i = 0; i < segments.size(); ++i) {
                content_rows.push_back(i < highlighted.size()
                        ? highlighted[i]
                        : text(segments[i]) | color(PANEL_FG));
            }
            Elements visual = diff_line_rows(5, false, number, std::nullopt,
                std::move(marker), std::move(content_rows));
            while (visual.size() < static_cast<std::size_t>(height)) {
                visual.push_back(text(""));
            }
            Element side
                = vbox(std::move(visual)) | size(WIDTH, EQUAL, side_width);
            return review_line_background(
                std::move(side), background, selected);
        }

        void _push_side_by_side_pair(Elements& rows,
            const ReviewSubsystem::Snapshot& snapshot, std::size_t file_index,
            const std::string& path, const ReviewLine* old_line,
            const ReviewLine* new_line, int side_width)
        {
            const ReviewLine* target
                = new_line != nullptr ? new_line : old_line;
            if (target == nullptr) {
                return;
            }
            const int content_width = review_side_content_width(side_width);
            const int height        = std::max(old_line != nullptr
                    ? static_cast<int>(
                          wrap_text(old_line->content, content_width).size())
                    : 1,
                new_line != nullptr
                    ? static_cast<int>(
                          wrap_text(new_line->content, content_width).size())
                    : 1);
            const bool selected
                = _selected == static_cast<int>(_visible.size());
            _push(
                rows,
                [this, old_line, new_line, side_width, height, selected] {
                    return hbox({
                        _side_line(
                            old_line, true, side_width, height, selected),
                        text(" │ ") | color(PANEL_BORDER),
                        _side_line(
                            new_line, false, side_width, height, selected),
                    });
                },
                VisibleRow { VisibleRow::Kind::LINE, file_index, target,
                    std::nullopt, 0 },
                selected, height);
            if (old_line != nullptr && old_line != target) {
                _push_comments(rows, snapshot, file_index, path, *old_line,
                    side_width * 2 + 3);
                _push_editor(rows, path, *old_line, side_width * 2 + 3);
            }
            _push_comments(
                rows, snapshot, file_index, path, *target, side_width * 2 + 3);
            _push_editor(rows, path, *target, side_width * 2 + 3);
        }

        void _push_side_by_side_hunk(Elements& rows,
            const ReviewSubsystem::Snapshot& snapshot, std::size_t file_index,
            const std::string& path, const ReviewHunk& hunk, int side_width)
        {
            std::size_t index = 0;
            while (index < hunk.lines.size()) {
                const ReviewLine& line = hunk.lines[index];
                if (line.kind == ReviewLine::Kind::DELETION) {
                    const std::size_t removed_begin = index;
                    while (index < hunk.lines.size()
                        && hunk.lines[index].kind
                            == ReviewLine::Kind::DELETION) {
                        ++index;
                    }
                    const std::size_t added_begin = index;
                    while (index < hunk.lines.size()
                        && hunk.lines[index].kind
                            == ReviewLine::Kind::ADDITION) {
                        ++index;
                    }
                    const std::size_t removed_count
                        = added_begin - removed_begin;
                    const std::size_t added_count = index - added_begin;
                    const std::size_t pair_count
                        = std::max(removed_count, added_count);
                    for (std::size_t offset = 0; offset < pair_count;
                        ++offset) {
                        const ReviewLine* old_line = offset < removed_count
                            ? &hunk.lines[removed_begin + offset]
                            : nullptr;
                        const ReviewLine* new_line = offset < added_count
                            ? &hunk.lines[added_begin + offset]
                            : nullptr;
                        _push_side_by_side_pair(rows, snapshot, file_index,
                            path, old_line, new_line, side_width);
                    }
                    continue;
                }
                if (line.kind == ReviewLine::Kind::ADDITION) {
                    _push_side_by_side_pair(rows, snapshot, file_index, path,
                        nullptr, &line, side_width);
                } else if (line.kind == ReviewLine::Kind::CONTEXT) {
                    _push_side_by_side_pair(rows, snapshot, file_index, path,
                        &line, &line, side_width);
                } else {
                    _push_unified_line(rows, snapshot, file_index, path, line,
                        side_width * 2 + 3);
                }
                ++index;
            }
        }

        bool _move(int delta)
        {
            if (_visible.empty()) {
                return false;
            }
            _selected = std::clamp(
                _selected + delta, 0, static_cast<int>(_visible.size()) - 1);
            _selected_comment = _visible[_selected].comment_id;
            return true;
        }

        bool _activate(bool mouse, bool open_editor = false)
        {
            if (_visible.empty()) {
                return false;
            }
            VisibleRow& row   = _visible[_selected];
            _selected_comment = row.comment_id;
            if (row.kind == VisibleRow::Kind::FILE) {
                return _collapse_selected();
            }
            if ((mouse || open_editor) && row.kind == VisibleRow::Kind::LINE) {
                return _open_editor();
            }
            return row.kind == VisibleRow::Kind::COMMENT;
        }

        bool _collapse_selected()
        {
            if (_visible.empty()) {
                return false;
            }
            const VisibleRow& row = _visible[_selected];
            _selected_comment     = row.comment_id;
            if (row.kind != VisibleRow::Kind::FILE) {
                return false;
            }
            const std::string& path = _path(row.file_index);
            if (_collapsed.contains(path)) {
                _collapsed.erase(path);
            } else {
                _collapsed.insert(path);
            }
            animation::RequestAnimationFrame();
            return true;
        }

        bool _button_press(const Mouse& mouse)
        {
            if (_plan_box.Contain(mouse.x, mouse.y)) {
                return _forward_press(_plan_button, mouse)
                    || (_send_to_plan(), true);
            }
            const ReviewSubsystem::Snapshot snapshot
                = _state->review->snapshot();
            if (_review_slot_box.Contain(mouse.x, mouse.y)) {
                if (snapshot.findings.task_id) {
                    return _forward_press(_review_cancel_button, mouse)
                        || (_cancel_review(), true);
                }
                return _forward_press(_review_action_button, mouse)
                    || (_provide_review(), true);
            }
            if (_handoff_slot_box.Contain(mouse.x, mouse.y)) {
                if (snapshot.handoff.task_id) {
                    return _forward_press(_handoff_cancel_button, mouse)
                        || (_cancel_handoff(), true);
                }
                return _forward_press(_handoff_action_button, mouse)
                    || (imza::run_handoff(*_state), true);
            }
            return false;
        }

        static bool _forward_press(Component& button, const Mouse& mouse)
        {
            Mouse press;
            press.button = Mouse::Left;
            press.motion = Mouse::Pressed;
            press.x      = mouse.x;
            press.y      = mouse.y;
            return button->OnEvent(Event::Mouse("", press));
        }

        bool _open_editor()
        {
            if (_visible.empty()) {
                return false;
            }
            const VisibleRow& row = _visible[_selected];
            if (row.kind != VisibleRow::Kind::LINE || row.line == nullptr) {
                return false;
            }
            _editing_comment.reset();
            _editor.begin(_anchor(_path(row.file_index), *row.line));
            return true;
        }

        bool _edit_comment()
        {
            if (!_selected_comment) {
                return false;
            }
            const auto snapshot = _state->review->snapshot();
            const auto it       = std::ranges::find(
                snapshot.comments, *_selected_comment, &ReviewComment::id);
            if (it == snapshot.comments.end()) {
                return false;
            }
            _editing_comment = it->id;
            _editor.begin_edit(static_cast<int>(it->id), it->body);
            return true;
        }

        bool _delete_comment()
        {
            if (!_selected_comment) {
                return false;
            }
            _state->review->delete_comment(*_selected_comment);
            _selected_comment.reset();
            return true;
        }

        void _save_editor()
        {
            if (!_editor.is_open() || _editor.draft().empty()) {
                return;
            }
            const std::string body = _editor.save();
            if (_editing_comment) {
                _state->review->update_comment(*_editing_comment, body);
            } else {
                _state->review->add_comment(_editor.anchor(), body);
            }
            _close_editor();
        }

        void _close_editor()
        {
            _editor.close();
            _editing_comment.reset();
        }

        bool _jump_file(int direction)
        {
            if (_visible.empty()) {
                return false;
            }
            const std::size_t current = _visible[_selected].file_index;
            if (direction > 0) {
                for (std::size_t i = _selected + 1; i < _visible.size(); ++i) {
                    if (_visible[i].kind == VisibleRow::Kind::FILE
                        && _visible[i].file_index > current) {
                        _selected = static_cast<int>(i);
                        return true;
                    }
                }
            } else {
                for (int i = _selected - 1; i >= 0; --i) {
                    if (_visible[i].kind == VisibleRow::Kind::FILE
                        && _visible[i].file_index < current) {
                        _selected = i;
                        return true;
                    }
                }
            }
            return true;
        }

        void _consume_jump(const ReviewSubsystem::Snapshot& snapshot)
        {
            if (snapshot.jump_file) {
                _collapsed.erase(*snapshot.jump_file);
                _pending_file_jump = *snapshot.jump_file;
                _state->review->clear_file_jump();
            }
            if (!snapshot.jump_comment) {
                return;
            }
            const auto comment = std::ranges::find(
                snapshot.comments, *snapshot.jump_comment, &ReviewComment::id);
            if (comment == snapshot.comments.end()) {
                _state->review->clear_jump();
                return;
            }
            _collapsed.erase(comment->anchor.file);
            _pending_jump = comment->id;
            _state->review->clear_jump();
        }

        const std::string& _path(std::size_t file_index) const
        {
            const ReviewFile& file = _rendered_review->files[file_index];
            return file.new_path.empty() ? file.old_path : file.new_path;
        }

        // The load runs on ReviewSubsystem's own worker; the tab only asks
        // for it and repaints when the state publishes.
        void _request_reload()
        {
            const auto workspace = _state->environment->workspace();
            if (!workspace || !workspace->project_root) {
                return;
            }
            _state->review->reload(*workspace->project_root);
            animation::RequestAnimationFrame();
        }
        Component _plan_button;
        Component _review_action_button;
        Component _review_cancel_button;
        Component _handoff_action_button;
        Component _handoff_cancel_button;

        std::shared_ptr<ApplicationState> _state;
        LayoutFn _layout;
        WorkflowNavigateFn _navigate;
        AnnotationEditor<ReviewLineAnchor> _editor;
        std::optional<std::size_t> _editing_comment;
        std::optional<std::size_t> _selected_comment;
        std::optional<std::size_t> _pending_jump;
        std::optional<std::string> _pending_file_jump;
        std::set<std::string> _collapsed;
        bool _load_requested = false;
        std::vector<VisibleRow> _visible;
        std::shared_ptr<const RepositoryReview> _rendered_review;
        ReviewHighlights _highlights;
        const RepositoryReview* _highlighted_review = nullptr;
        std::deque<Box> _boxes;
        std::vector<int> _box_rows;
        Box _plan_box;
        Box _review_slot_box;
        Box _handoff_slot_box;
        int _selected                  = 0;
        int _rendered_y                = 0;
        int _skipped_height            = 0;
        int _render_radius             = 120;
        int _highlighted_width         = 0;
        bool _highlighted_side_by_side = false;
        Signal<>::Subscription _repository_subscription;
        Signal<>::Subscription _workspace_subscription;
        Signal<>::Subscription _review_subscription;
    };

} // namespace

Component make_review(std::shared_ptr<ApplicationState> state, LayoutFn layout,
    WorkflowNavigateFn navigate)
{
    return ftxui::Make<Review>(
        std::move(state), std::move(layout), std::move(navigate));
}

} // namespace imza
