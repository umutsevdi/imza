#include "agent/flows.h"
#include "common/types.h"
#include "common/util.h"
#include "subsystems/delegation_runner.h"
#include "subsystems/review.h"
#include "ui/ui.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/string.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <format>
#include <set>
#include <thread>

namespace ursa {
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

    class Review : public ComponentBase {
    public:
        Review(std::shared_ptr<ApplicationState> state, LayoutFn layout,
            WorkflowNavigateFn navigate)
            : state_(std::move(state))
            , layout_(std::move(layout))
            , navigate_(std::move(navigate))
            , comment_input_(Input(&draft_,
                  multiline_field_option(
                      &draft_, &draft_cursor_, "Leave a comment")))
            , repository_subscription_(
                  state_->environment->subscribe_to_repository_change([this] {
                      load_generation_->fetch_add(1);
                      reload_pending_.store(true);
                      animation::RequestAnimationFrame();
                  }))
            , workspace_subscription_(
                  state_->environment->subscribe_to_workspace_change([this] {
                      load_generation_->fetch_add(1);
                      reload_pending_.store(true);
                      animation::RequestAnimationFrame();
                  }))
            , review_subscription_(state_->review->subscribe(
                  [] { animation::RequestAnimationFrame(); }))
        {
            plan_button_
                = action_button("Send to Plan", [this] { _send_to_plan(); });
            ai_review_button_
                = action_button("AI Review", [this] { _provide_review(); });
            ButtonOption viewer_option;
            viewer_option.label     = "Reviewing…";
            viewer_option.on_click  = [this] { _open_review_viewer(); };
            viewer_option.transform = [this](const EntryState& entry) {
                Element label
                    = text(entry.label + " " + _review_elapsed_text());
                if (entry.focused) {
                    label = std::move(label) | bold | underlined
                        | color(PANEL_FG);
                } else {
                    label = std::move(label) | color(PANEL_FG_DIM);
                }
                return hbox(
                    { spinner(15, static_cast<std::size_t>(review_frame_))
                            | color(Color::GrayLight),
                        text(" "), std::move(label) });
            };
            review_viewer_button_ = space_activates(
                Button(viewer_option), viewer_option.on_click);
            ButtonOption cancel_option;
            cancel_option.label     = "cancel";
            cancel_option.on_click  = [this] { _cancel_review(); };
            cancel_option.transform = [this](const EntryState& entry) {
                Element label = text(
                    review_cancelling_->load() ? "cancelling…" : entry.label);
                if (entry.focused) {
                    label = std::move(label) | bold | underlined
                        | color(PANEL_FG);
                } else {
                    label = std::move(label) | color(PANEL_FG_DIM);
                }
                return label;
            };
            review_cancel_button_ = space_activates(
                Button(cancel_option), cancel_option.on_click);
            Add(comment_input_);
            Add(plan_button_);
            Add(ai_review_button_);
            Add(review_viewer_button_);
            Add(review_cancel_button_);
        }

        ~Review() override { load_generation_->fetch_add(1); }

        Element OnRender() override
        {
            if (reload_pending_.load() && !load_running_->load()) {
                reload_pending_.store(false);
                _reload();
            }
            const ReviewState::Snapshot snapshot = state_->review->snapshot();
            _consume_jump(snapshot);
            visible_.clear();
            boxes_.clear();
            box_rows_.clear();
            rendered_y_       = 0;
            skipped_height_   = 0;
            horizontal_limit_ = 0;

            if (snapshot.status == ReviewState::LoadStatus::IDLE
                || snapshot.status == ReviewState::LoadStatus::LOADING) {
                return center(text("Loading changes…") | color(PANEL_FG_DIM))
                    | flex;
            }
            if (snapshot.status == ReviewState::LoadStatus::ERROR) {
                return center(vbox({ text("Unable to load changes") | bold,
                           paragraph(snapshot.error) | color(PANEL_FG_DIM) }))
                    | flex;
            }
            rendered_review_ = snapshot.review;
            if (snapshot.review->files.empty()) {
                return center(
                           text("Working tree is clean") | color(PANEL_FG_DIM))
                    | flex;
            }
            horizontal_limit_ = _horizontal_limit(*snapshot.review);
            horizontal_offset_
                = std::clamp(horizontal_offset_, 0, horizontal_limit_);

            Elements rows;
            for (std::size_t file_index = 0;
                file_index < snapshot.review->files.size(); ++file_index) {
                const ReviewFile& file = snapshot.review->files[file_index];
                _push_file(rows, snapshot, file, file_index);
                if (file_index + 1 < snapshot.review->files.size()) {
                    _flush_spacer(rows);
                    rows.push_back(separatorEmpty());
                    ++rendered_y_;
                }
            }
            if (pending_jump_) {
                for (std::size_t i = 0; i < visible_.size(); ++i) {
                    if (visible_[i].comment_id == pending_jump_) {
                        selected_         = static_cast<int>(i);
                        selected_comment_ = pending_jump_;
                        pending_jump_.reset();
                        animation::RequestAnimationFrame();
                        break;
                    }
                }
            }
            if (pending_file_jump_) {
                for (std::size_t i = 0; i < visible_.size(); ++i) {
                    if (visible_[i].kind == VisibleRow::Kind::FILE
                        && _path(visible_[i].file_index)
                            == *pending_file_jump_) {
                        selected_ = static_cast<int>(i);
                        selected_comment_.reset();
                        animation::RequestAnimationFrame();
                        break;
                    }
                }
                pending_file_jump_.reset();
            }
            if (visible_.empty()) {
                return text("") | flex;
            }
            selected_ = std::clamp(
                selected_, 0, static_cast<int>(visible_.size()) - 1);

            const int selected_y = visible_[selected_].display_y;
            Element content      = vbox(std::move(rows))
                | focusPosition(0, selected_y) | yframe | vscroll_indicator
                | flex;
            const std::string hint = editor_anchor_
                ? "Enter save · Alt+Enter new line · Esc cancel"
                : selected_comment_ ? "↑↓ navigate · e edit · d delete"
                : horizontal_limit_ > 0
                ? "↑↓ navigate · ←→ scroll · [] files · Enter collapse · c "
                  "comment"
                : "↑↓ navigate · [] files · Enter collapse · c comment";
            Elements bottom { };

            if (!state_->session->error().empty()
                || state_->session->retry_countdown()) {
                bottom.push_back(session_error_element(*state_->session));
            }
            const bool review_running = review_running_->load();
            Element plan_action       = plan_button_->Render();
            Element review_action     = ai_review_button_->Render();
            if (review_running) {
                plan_action   = std::move(plan_action) | dim;
                review_action = std::move(review_action) | dim;
            }
            Elements actions { std::move(plan_action), text(" "),
                std::move(review_action) };
            if (review_running) {
                animation::RequestAnimationFrame();
                actions.push_back(text(" "));
                actions.push_back(review_viewer_button_->Render());
                actions.push_back(text(" · "));
                actions.push_back(review_cancel_button_->Render());
            }
            actions.push_back(filler());
            actions.push_back(text(hint) | dim);
            bottom.push_back(hbox(std::move(actions)));
            return vbox({ std::move(content), vbox(std::move(bottom)) }) | flex;
        }

        void OnAnimation(animation::Params&) override
        {
            if (review_running_->load()) {
                ++review_frame_;
                animation::RequestAnimationFrame();
            }
        }

        bool OnEvent(Event event) override
        {
            if (!state_->session->error().empty()
                && _is_user_interaction(event)) {
                state_->session->clear_error();
            }
            if (editor_anchor_) {
                if (event == Event::Escape) {
                    _close_editor();
                    return true;
                }
                if (event == Event::Special("\x1B\r")
                    || event == Event::Special("\x1B\n")) {
                    draft_.insert(
                        static_cast<std::size_t>(draft_cursor_), "\n");
                    ++draft_cursor_;
                    animation::RequestAnimationFrame();
                    return true;
                }
                if (event == Event::Return) {
                    _save_editor();
                    return true;
                }
                return comment_input_->OnEvent(event);
            }
            if (plan_button_->OnEvent(event)
                || ai_review_button_->OnEvent(event)) {
                return true;
            }
            if (review_running_->load()
                && review_viewer_button_->OnEvent(event)) {
                return true;
            }
            if (review_running_->load()
                && review_cancel_button_->OnEvent(event)) {
                return true;
            }
            if (event.is_mouse()) {
                const Mouse& mouse = event.mouse();
                if (mouse.button == Mouse::WheelUp) {
                    return _move(-3);
                }
                if (mouse.button == Mouse::WheelDown) {
                    return _move(3);
                }
                if (mouse.button == Mouse::Left
                    && mouse.motion == Mouse::Pressed) {
                    for (std::size_t i = 0; i < boxes_.size(); ++i) {
                        if (boxes_[i].Contain(mouse.x, mouse.y)) {
                            selected_ = box_rows_[i];
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
            if (event == Event::ArrowLeft) {
                return _scroll_horizontal(-4);
            }
            if (event == Event::ArrowRight) {
                return _scroll_horizontal(4);
            }
            if (event == Event::PageUp) {
                return _move(-10);
            }
            if (event == Event::PageDown) {
                return _move(10);
            }
            if (event == Event::Home) {
                selected_ = 0;
                return true;
            }
            if (event == Event::End) {
                selected_ = std::max(0, static_cast<int>(visible_.size()) - 1);
                return true;
            }
            if (event == Event::Character("[")) {
                return _jump_file(-1);
            }
            if (event == Event::Character("]")) {
                return _jump_file(1);
            }
            if (event == Event::Return || event == Event::Character(" ")) {
                return _activate(false);
            }
            if (event == Event::Character("c")) {
                return _open_editor();
            }
            if (event == Event::Character("e")) {
                return _edit_comment();
            }
            if (event == Event::Character("d")) {
                return _delete_comment();
            }
            return false;
        }

    private:
        static constexpr int RENDER_RADIUS = 120;

        static bool _is_user_interaction(Event event)
        {
            const bool mouse_input = event.is_mouse()
                && (event.mouse().motion == Mouse::Pressed
                    || event.mouse().button == Mouse::WheelUp
                    || event.mouse().button == Mouse::WheelDown);
            return mouse_input || event.is_character()
                || event == Event::Backspace || event == Event::Delete
                || event == Event::Return || event == Event::Escape
                || event == Event::ArrowUp || event == Event::ArrowDown
                || event == Event::ArrowLeft || event == Event::ArrowRight
                || event == Event::PageUp || event == Event::PageDown
                || event == Event::Home || event == Event::End
                || event == Event::Special("\x1B\r")
                || event == Event::Special("\x1B\n");
        }

        void _send_to_plan()
        {
            if (review_running_->load()) {
                return;
            }
            const auto snapshot = state_->review->comments_snapshot();
            if (snapshot.comments.empty()) {
                state_->session->set_error(
                    "Add a review comment before sending.");
                return;
            }
            if (!state_->providers->active_selection()) {
                state_->session->set_error("No model selected — run /model.");
                return;
            }
            std::string prompt = format_review_plan_prompt(snapshot.comments);
            navigate_(WorkflowPhase::PLAN);
            ursa::submit(*state_, std::move(prompt));
            state_->review->clear_comments();
            selected_comment_.reset();
            pending_jump_.reset();
        }

        void _provide_review()
        {
            if (review_running_->exchange(true)) {
                return;
            }
            review_cancelling_->store(false);
            review_frame_        = 0;
            review_started_      = std::chrono::steady_clock::now();
            const auto selection = state_->providers->active_selection();
            if (!selection) {
                review_running_->store(false);
                state_->session->set_error("No model selected — run /model.");
                return;
            }
            const ReviewState::Snapshot snapshot = state_->review->snapshot();
            if (snapshot.status != ReviewState::LoadStatus::LOADED
                || snapshot.review->files.empty()) {
                review_running_->store(false);
                state_->session->set_error("There are no changes to review.");
                return;
            }
            std::string prompt
                = format_ai_review_prompt(*snapshot.review, snapshot.comments);
            constexpr std::size_t MAX_REVIEW_PROMPT_BYTES = 200 * 1024;
            if (prompt.size() > MAX_REVIEW_PROMPT_BYTES) {
                review_running_->store(false);
                state_->session->set_error(
                    "AI review is too large (limit: 200 KiB). Reduce the diff "
                    "or review it in smaller commits.");
                return;
            }
            auto transcript             = std::make_shared<Session>();
            const SubagentHandle handle = state_->delegation->run_subagent(
                std::move(prompt), selection->model, "low",
                SubagentOptions { .visible = false,
                    .timeout               = std::chrono::minutes { 5 },
                    .max_output_tokens     = 4096,
                    .transcript            = std::move(transcript) },
                [state = state_, running = review_running_](
                    const SubagentResult& result) {
                    running->store(false);
                    if (result.status == Status::CANCELLED) {
                        return;
                    }
                    if (result.status != Status::OK) {
                        state->session->set_error(
                            "AI review failed: " + error_text(result.status));
                        return;
                    }
                    const ReviewState::Snapshot current
                        = state->review->snapshot();
                    AiReviewParseResult parsed = parse_ai_review_response(
                        result.output, *current.review);
                    if (auto* error = std::get_if<std::string>(&parsed)) {
                        state->session->set_error(std::move(*error));
                        return;
                    }
                    state->review->add_comments(std::move(
                        std::get<std::vector<ReviewCommentDraft>>(parsed)));
                });
            review_task_id_ = handle.id;
        }

        void _cancel_review()
        {
            if (!review_task_id_ || review_cancelling_->exchange(true)) {
                return;
            }
            if (!state_->subagents->cancel(*review_task_id_)) {
                review_cancelling_->store(false);
            }
        }

        void _open_review_viewer()
        {
            if (!review_task_id_) {
                return;
            }
            SubagentChat chat = state_->delegation->subagent_chat(
                *review_task_id_, "AI Review");
            ursa::enqueue_user_modal(*state_,
                ViewerModal { std::move(chat.title), std::move(chat.transcript),
                    "markdown", 1, true, "" });
        }

        std::string _review_elapsed_text() const
        {
            return elapsed_text(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - review_started_));
        }

        struct VisibleRow {
            enum class Kind { FILE, HUNK, LINE, COMMENT };
            Kind kind              = Kind::HUNK;
            std::size_t file_index = 0;
            const ReviewLine* line = nullptr;
            std::optional<std::size_t> comment_id;
            int display_y = 0;
        };

        void _flush_spacer(Elements& rows)
        {
            if (skipped_height_ == 0) {
                return;
            }
            rows.push_back(text("") | size(HEIGHT, EQUAL, skipped_height_));
            skipped_height_ = 0;
        }

        template <typename Render>
        void _push(Elements& rows, Render&& render, VisibleRow visible,
            bool selected, int height = 1)
        {
            const int row_index = static_cast<int>(visible_.size());
            visible.display_y   = rendered_y_;
            rendered_y_ += height;
            visible_.push_back(std::move(visible));
            if (std::abs(row_index - selected_) > RENDER_RADIUS) {
                skipped_height_ += height;
                return;
            }
            _flush_spacer(rows);
            Element row = render();
            if (selected) {
                row = std::move(row) | bgcolor(PANEL_COLOR_FOCUS);
            }
            boxes_.push_back(Box { });
            box_rows_.push_back(row_index);
            rows.push_back(std::move(row) | xflex | reflect(boxes_.back()));
        }

        void _push_file(Elements& rows, const ReviewState::Snapshot& snapshot,
            const ReviewFile& file, std::size_t file_index)
        {
            Elements file_rows;
            const std::string& path
                = file.new_path.empty() ? file.old_path : file.new_path;
            const bool collapsed       = collapsed_.contains(path);
            const std::size_t comments = std::ranges::count_if(
                snapshot.comments, [&path](const ReviewComment& comment) {
                    return comment.anchor.file == path;
                });
            Elements header_parts {
                text(collapsed ? "› " : "⌄ ") | color(PANEL_FG_DIM),
                text(path) | bold | color(PANEL_FG),
                filler(),
                text("+" + std::to_string(file.additions))
                    | color(Color::GreenLight),
                text(" "),
                text("−" + std::to_string(file.deletions))
                    | color(Color::RedLight),
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
                selected_ == static_cast<int>(visible_.size()));
            if (!collapsed && file.kind == ReviewFile::Kind::BINARY) {
                _push(
                    file_rows,
                    [] {
                        return text("  Binary file changed")
                            | color(PANEL_FG_DIM);
                    },
                    VisibleRow { VisibleRow::Kind::HUNK, file_index, nullptr,
                        std::nullopt, 0 },
                    selected_ == static_cast<int>(visible_.size()));
            } else if (!collapsed) {
                const LayoutCtx ctx     = layout_();
                const int review_width  = ctx.kind == LayoutCtx::Kind::WIDE
                    ? ctx.width - LayoutCtx::panel_width - 4
                    : ctx.width;
                const bool side_by_side = review_width >= 100;
                const int side_width    = std::max(20, (review_width - 3) / 2);
                for (const ReviewHunk& hunk : file.hunks) {
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
                        selected_ == static_cast<int>(visible_.size()));
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
            const ReviewState::Snapshot& snapshot, std::size_t file_index,
            const std::string& path, const ReviewLine& line)
        {
            for (const ReviewComment& comment : snapshot.comments) {
                if (!_matches(comment.anchor, path, line)) {
                    continue;
                }
                _push(
                    rows,
                    [&comment] {
                        Elements body;
                        for (const std::string& text_line :
                            split_lines(comment.body)) {
                            body.push_back(text(text_line) | color(PANEL_FG));
                        }
                        const std::size_t line_count
                            = std::ranges::count(comment.body, '\n') + 1;
                        while (body.size() < line_count) {
                            body.push_back(text("") | color(PANEL_FG));
                        }
                        return hbox({ text("             "),
                                   vbox(std::move(body)) | xflex, filler() })
                            | bgcolor(PANEL_BORDER);
                    },
                    VisibleRow { VisibleRow::Kind::COMMENT, file_index, nullptr,
                        comment.id, 0 },
                    selected_ == static_cast<int>(visible_.size()),
                    static_cast<int>(
                        std::ranges::count(comment.body, '\n') + 1));
            }
        }

        void _push_editor(
            Elements& rows, const std::string& path, const ReviewLine& line)
        {
            if (!editor_anchor_ || !_matches(*editor_anchor_, path, line)) {
                return;
            }
            _flush_spacer(rows);
            rows.push_back(hbox({ text("               "),
                comment_input_->Render() | xflex, text(" ") }));
            rendered_y_
                += static_cast<int>(std::ranges::count(draft_, '\n') + 1);
        }

        void _push_unified_line(Elements& rows,
            const ReviewState::Snapshot& snapshot, std::size_t file_index,
            const std::string& path, const ReviewLine& line, int review_width)
        {
            const std::string syntax = syntax_type_for_path(path);
            const auto number = [](const std::optional<std::size_t>& value) {
                return value ? std::format("{:>5}", *value)
                             : std::string(5, ' ');
            };
            std::string marker = " ";
            std::optional<Color> background;
            if (line.kind == ReviewLine::Kind::ADDITION) {
                marker     = "+";
                background = DIFF_ADDITION_BG;
            } else if (line.kind == ReviewLine::Kind::DELETION) {
                marker     = "−";
                background = DIFF_DELETION_BG;
            }
            _push(
                rows,
                [this, &line, marker = std::move(marker), background, number,
                    review_width, syntax] {
                    const int content_width = std::max(1, review_width - 14);
                    const std::string content
                        = fit(line.content, content_width, horizontal_offset_);
                    Element row = hbox({
                        text(number(line.old_line)) | color(PANEL_FG_DIM),
                        text(" "),
                        text(number(line.new_line)) | color(PANEL_FG_DIM),
                        text(" "),
                        text(marker + " "),
                        line.kind == ReviewLine::Kind::META
                            ? text(content) | color(PANEL_FG_DIM)
                            : highlight_code_line(content, syntax),
                    });
                    if (background) {
                        row = std::move(row) | bgcolor(*background);
                    }
                    return row;
                },
                VisibleRow { VisibleRow::Kind::LINE, file_index, &line,
                    std::nullopt, 0 },
                selected_ == static_cast<int>(visible_.size()));
            _push_comments(rows, snapshot, file_index, path, line);
            _push_editor(rows, path, line);
        }

        Element _side_line(const ReviewLine* line, bool old_side,
            int side_width, std::string_view syntax) const
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
            std::string marker = " ";
            std::optional<Color> background;
            if (line->kind == ReviewLine::Kind::DELETION) {
                marker     = "−";
                background = DIFF_DELETION_BG;
            } else if (line->kind == ReviewLine::Kind::ADDITION) {
                marker     = "+";
                background = DIFF_ADDITION_BG;
            }
            const std::string content = fit(
                line->content, std::max(1, side_width - 8), horizontal_offset_);
            Element side = hbox({
                               text(number_text) | color(PANEL_FG_DIM),
                               text(" "),
                               text(marker + " "),
                               highlight_code_line(content, syntax) | xflex,
                           })
                | size(WIDTH, EQUAL, side_width);
            if (background) {
                side = std::move(side) | bgcolor(*background);
            }
            return side;
        }

        void _push_side_by_side_pair(Elements& rows,
            const ReviewState::Snapshot& snapshot, std::size_t file_index,
            const std::string& path, const ReviewLine* old_line,
            const ReviewLine* new_line, int side_width)
        {
            const std::string syntax = syntax_type_for_path(path);
            const ReviewLine* target
                = new_line != nullptr ? new_line : old_line;
            if (target == nullptr) {
                return;
            }
            _push(
                rows,
                [this, old_line, new_line, side_width, syntax] {
                    return hbox({
                        _side_line(old_line, true, side_width, syntax),
                        text(" │ ") | color(PANEL_BORDER),
                        _side_line(new_line, false, side_width, syntax),
                    });
                },
                VisibleRow { VisibleRow::Kind::LINE, file_index, target,
                    std::nullopt, 0 },
                selected_ == static_cast<int>(visible_.size()));
            if (old_line != nullptr && old_line != target) {
                _push_comments(rows, snapshot, file_index, path, *old_line);
                _push_editor(rows, path, *old_line);
            }
            _push_comments(rows, snapshot, file_index, path, *target);
            _push_editor(rows, path, *target);
        }

        void _push_side_by_side_hunk(Elements& rows,
            const ReviewState::Snapshot& snapshot, std::size_t file_index,
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
            if (visible_.empty()) {
                return false;
            }
            selected_ = std::clamp(
                selected_ + delta, 0, static_cast<int>(visible_.size()) - 1);
            selected_comment_ = visible_[selected_].comment_id;
            return true;
        }

        bool _scroll_horizontal(int delta)
        {
            if (horizontal_limit_ == 0) {
                return false;
            }
            horizontal_offset_
                = std::clamp(horizontal_offset_ + delta, 0, horizontal_limit_);
            return true;
        }

        int _horizontal_limit(const RepositoryReview& review) const
        {
            const LayoutCtx ctx             = layout_();
            const int review_width          = ctx.kind == LayoutCtx::Kind::WIDE
                ? ctx.width - LayoutCtx::panel_width - 4
                : ctx.width;
            const bool side_by_side         = review_width >= 100;
            const int side_content_width    = side_by_side
                ? std::max(1, std::max(20, (review_width - 3) / 2) - 8)
                : std::max(1, review_width - 14);
            const int unified_content_width = std::max(1, review_width - 14);
            int limit                       = 0;
            for (const ReviewFile& file : review.files) {
                const std::string& path
                    = file.new_path.empty() ? file.old_path : file.new_path;
                if (collapsed_.contains(path)) {
                    continue;
                }
                for (const ReviewHunk& hunk : file.hunks) {
                    for (const ReviewLine& line : hunk.lines) {
                        const bool split_line = side_by_side
                            && line.kind != ReviewLine::Kind::META;
                        const int content_width = split_line
                            ? side_content_width
                            : unified_content_width;
                        limit                   = std::max(
                            limit, string_width(line.content) - content_width);
                    }
                }
            }
            return std::max(0, limit);
        }

        bool _activate(bool mouse)
        {
            if (visible_.empty()) {
                return false;
            }
            VisibleRow& row   = visible_[selected_];
            selected_comment_ = row.comment_id;
            if (row.kind == VisibleRow::Kind::FILE) {
                const std::string& path = _path(row.file_index);
                if (collapsed_.contains(path)) {
                    collapsed_.erase(path);
                } else {
                    collapsed_.insert(path);
                }
                animation::RequestAnimationFrame();
                return true;
            }
            if (mouse && row.kind == VisibleRow::Kind::LINE) {
                return _open_editor();
            }
            return row.kind == VisibleRow::Kind::COMMENT;
        }

        bool _open_editor()
        {
            if (visible_.empty()) {
                return false;
            }
            const VisibleRow& row = visible_[selected_];
            if (row.kind != VisibleRow::Kind::LINE || row.line == nullptr) {
                return false;
            }
            editor_anchor_ = _anchor(_path(row.file_index), *row.line);
            editing_comment_.reset();
            draft_.clear();
            draft_cursor_ = 0;
            comment_input_->TakeFocus();
            return true;
        }

        bool _edit_comment()
        {
            if (!selected_comment_) {
                return false;
            }
            const auto snapshot = state_->review->snapshot();
            const auto it       = std::ranges::find(
                snapshot.comments, *selected_comment_, &ReviewComment::id);
            if (it == snapshot.comments.end()) {
                return false;
            }
            editor_anchor_   = it->anchor;
            editing_comment_ = it->id;
            draft_           = it->body;
            draft_cursor_    = static_cast<int>(draft_.size());
            comment_input_->TakeFocus();
            return true;
        }

        bool _delete_comment()
        {
            if (!selected_comment_) {
                return false;
            }
            state_->review->delete_comment(*selected_comment_);
            selected_comment_.reset();
            return true;
        }

        void _save_editor()
        {
            if (!editor_anchor_ || draft_.empty()) {
                return;
            }
            if (editing_comment_) {
                state_->review->update_comment(*editing_comment_, draft_);
            } else {
                state_->review->add_comment(*editor_anchor_, draft_);
            }
            _close_editor();
        }

        void _close_editor()
        {
            editor_anchor_.reset();
            editing_comment_.reset();
            draft_.clear();
            draft_cursor_ = 0;
        }

        bool _jump_file(int direction)
        {
            if (visible_.empty()) {
                return false;
            }
            const std::size_t current = visible_[selected_].file_index;
            if (direction > 0) {
                for (std::size_t i = selected_ + 1; i < visible_.size(); ++i) {
                    if (visible_[i].kind == VisibleRow::Kind::FILE
                        && visible_[i].file_index > current) {
                        selected_ = static_cast<int>(i);
                        return true;
                    }
                }
            } else {
                for (int i = selected_ - 1; i >= 0; --i) {
                    if (visible_[i].kind == VisibleRow::Kind::FILE
                        && visible_[i].file_index < current) {
                        selected_ = i;
                        return true;
                    }
                }
            }
            return true;
        }

        void _consume_jump(const ReviewState::Snapshot& snapshot)
        {
            if (snapshot.jump_file) {
                collapsed_.erase(*snapshot.jump_file);
                pending_file_jump_ = *snapshot.jump_file;
                state_->review->clear_file_jump();
            }
            if (!snapshot.jump_comment) {
                return;
            }
            const auto comment = std::ranges::find(
                snapshot.comments, *snapshot.jump_comment, &ReviewComment::id);
            if (comment == snapshot.comments.end()) {
                state_->review->clear_jump();
                return;
            }
            collapsed_.erase(comment->anchor.file);
            pending_jump_ = comment->id;
            state_->review->clear_jump();
        }

        const std::string& _path(std::size_t file_index) const
        {
            const ReviewFile& file = rendered_review_->files[file_index];
            return file.new_path.empty() ? file.old_path : file.new_path;
        }

        void _reload()
        {
            const auto workspace = state_->environment->workspace();
            if (!workspace || !workspace->project_root) {
                return;
            }
            if (load_running_->exchange(true)) {
                reload_pending_.store(true);
                return;
            }
            const std::filesystem::path root = *workspace->project_root;
            state_->review->set_loading();
            const std::uint64_t generation = load_generation_->load();
            std::thread([review             = state_->review, root,
                            load_generation = load_generation_,
                            load_running    = load_running_, generation] {
                ReviewLoadResult result = load_repository_review(root);
                if (load_generation->load() == generation) {
                    review->set_result(std::move(result));
                }
                load_running->store(false);
                animation::RequestAnimationFrame();
            }).detach();
        }
        Component plan_button_;
        Component ai_review_button_;
        Component review_viewer_button_;
        Component review_cancel_button_;

        std::shared_ptr<ApplicationState> state_;
        LayoutFn layout_;
        WorkflowNavigateFn navigate_;
        Component comment_input_;
        std::string draft_;
        int draft_cursor_ = 0;
        std::optional<ReviewLineAnchor> editor_anchor_;
        std::optional<std::size_t> editing_comment_;
        std::optional<std::size_t> selected_comment_;
        std::optional<std::size_t> pending_jump_;
        std::optional<std::size_t> review_task_id_;
        std::optional<std::string> pending_file_jump_;
        std::set<std::string> collapsed_;
        std::vector<VisibleRow> visible_;
        std::shared_ptr<const RepositoryReview> rendered_review_;
        std::deque<Box> boxes_;
        std::vector<int> box_rows_;
        int selected_          = 0;
        int rendered_y_        = 0;
        int skipped_height_    = 0;
        int horizontal_offset_ = 0;
        int horizontal_limit_  = 0;
        int review_frame_      = 0;
        std::chrono::steady_clock::time_point review_started_;
        std::shared_ptr<std::atomic<std::uint64_t>> load_generation_
            = std::make_shared<std::atomic<std::uint64_t>>(0);
        std::shared_ptr<std::atomic<bool>> load_running_
            = std::make_shared<std::atomic<bool>>(false);
        std::shared_ptr<std::atomic<bool>> review_running_
            = std::make_shared<std::atomic<bool>>(false);
        std::shared_ptr<std::atomic<bool>> review_cancelling_
            = std::make_shared<std::atomic<bool>>(false);
        std::atomic<bool> reload_pending_ { true };
        Signal<>::Subscription repository_subscription_;
        Signal<>::Subscription workspace_subscription_;
        Signal<>::Subscription review_subscription_;
    };

} // namespace

Component make_review(std::shared_ptr<ApplicationState> state, LayoutFn layout,
    WorkflowNavigateFn navigate)
{
    return ftxui::Make<Review>(
        std::move(state), std::move(layout), std::move(navigate));
}

} // namespace ursa
