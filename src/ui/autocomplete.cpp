#include "ui/autocomplete.h"

#include "agent/application_state.h"
#include "agent/subsystems/skill_store.h"
#include "environment/environment.h"
#include "provider/provider_store.h"
#include "ui/ui.h"
#include "common/util.h"

#include <algorithm>
#include <filesystem>

namespace ursa {

int Autocomplete::count() const
{
    if (!files_.empty()) {
        return static_cast<int>(files_.size());
    }
    if (!skills_.empty()) {
        return static_cast<int>(skills_.size());
    }
    return static_cast<int>(commands_.size());
}

bool Autocomplete::active() const { return count() > 0; }

bool Autocomplete::handle_event(const ftxui::Event& event)
{
    const int n = count();
    if (n == 0) {
        return false;
    }
    if (event == ftxui::Event::ArrowDown) {
        selected_ = (selected_ + 1) % n;
        return true;
    }
    if (event == ftxui::Event::ArrowUp) {
        selected_ = (selected_ - 1 + n) % n;
        return true;
    }
    return false;
}

void Autocomplete::refresh(
    const ApplicationState& state, const std::string& text, int cursor)
{
    clear();
    const std::size_t at = static_cast<std::size_t>(cursor);
    token_               = attachment_token_at(text, at);
    if (token_) {
        files_ = attachment_candidates(
            std::filesystem::current_path(), token_->query);
        return;
    }
    const std::size_t begin = word_begin(text, at);
    if (begin < at && text[begin] == '$' && mention_end(text, begin) >= at) {
        skill_begin_ = begin;
        const std::string key
            = to_lower(text.substr(begin + 1, at - begin - 1));
        const std::vector<Skill> allowed = allowed_skills(
            state.environment->skills(), state.providers->config());
        for (const Skill& skill : allowed) {
            const std::string name = to_lower(skill.name);
            if (name.starts_with(key)) {
                skills_.push_back(skill);
            }
        }
        return;
    }
    skill_begin_.reset();
    if (text.empty() || text[0] != '/' || text.find(' ') != std::string::npos) {
        return;
    }
    const std::string key = to_lower(text);
    for (const auto& c : slash_commands()) {
        const std::string name = to_lower(c.name);
        if (name.size() >= key.size()
            && name.compare(0, key.size(), key) == 0) {
            commands_.push_back(&c);
        }
    }
    if (commands_.size() == 1 && commands_[0]->name == text) {
        commands_.clear();
    }
}

bool Autocomplete::accept(const ApplicationState& state, std::string& text,
    int& cursor, std::vector<FileAttachment>& attachments)
{
    if (!files_.empty() && token_) {
        const AttachmentCandidate& candidate
            = files_[static_cast<std::size_t>(selected_)];
        const std::string replacement = "@" + candidate.path;
        text.replace(token_->begin, token_->end - token_->begin, replacement);
        cursor = static_cast<int>(token_->begin + replacement.size());
        if (candidate.directory) {
            refresh(state, text, cursor);
            return false;
        }
        AttachmentResult loaded
            = load_attachment(std::filesystem::current_path(), candidate.path);
        if (!loaded.attachment) {
            state.session->set_error(std::move(loaded.error));
            refresh(state, text, cursor);
            return false;
        }
        const auto duplicate = std::find_if(attachments.begin(),
            attachments.end(), [&](const FileAttachment& attachment) {
                return attachment.path == loaded.attachment->path;
            });
        if (duplicate == attachments.end()) {
            std::string error;
            if (!can_add_attachment(attachments, *loaded.attachment, error)) {
                state.session->set_error(std::move(error));
                refresh(state, text, cursor);
                return false;
            }
            attachments.push_back(std::move(*loaded.attachment));
        }
        text.insert(static_cast<std::size_t>(cursor), " ");
        ++cursor;
        clear();
        return false;
    }
    if (!skills_.empty() && skill_begin_) {
        const std::string replacement
            = "$" + skills_[static_cast<std::size_t>(selected_)].name;
        text.replace(*skill_begin_,
            static_cast<std::size_t>(cursor) - *skill_begin_, replacement);
        cursor = static_cast<int>(*skill_begin_ + replacement.size());
        text.insert(static_cast<std::size_t>(cursor), " ");
        ++cursor;
        refresh(state, text, cursor);
        return false;
    }
    const SlashCommand* cmd = commands_[static_cast<std::size_t>(selected_)];
    text                    = cmd->name;
    cursor                  = static_cast<int>(text.size());
    refresh(state, text, cursor);
    return true;
}

void Autocomplete::clear()
{
    commands_.clear();
    skills_.clear();
    files_.clear();
    token_.reset();
    skill_begin_.reset();
    selected_ = 0;
}

ftxui::Element Autocomplete::render(const LayoutCtx& ctx) const
{
    using namespace ftxui;
    const size_t max_rows       = 8;
    const int available_width   = ctx.kind == LayoutCtx::Kind::WIDE
        ? ctx.width - LayoutCtx::panel_width
        : ctx.width;
    const int description_width = std::clamp(available_width - 28, 8, 56);
    const size_t total          = !files_.empty() ? files_.size()
        : !skills_.empty()                        ? skills_.size()
                                                  : commands_.size();
    const size_t shown          = std::min(total, max_rows);
    const size_t selected       = static_cast<size_t>(std::max(0, selected_));
    const size_t first
        = selected < shown ? 0 : std::min(selected - shown + 1, total - shown);
    Elements rows;
    if (first > 0) {
        rows.push_back(text("  ↑ " + std::to_string(first) + " more") | dim
            | color(PANEL_FG_DIM));
    }
    for (size_t row_index = 0; row_index < shown; ++row_index) {
        const size_t i              = first + row_index;
        const bool sel              = static_cast<int>(i) == selected_;
        const std::string name_text = !files_.empty() ? "@" + files_[i].path
            : !skills_.empty() ? "$" + skills_[i].name
                               : std::string(commands_[i]->name);
        Element name                = text(name_text);
        if (sel) {
            name = name | bold;
        }
        const std::string description = !files_.empty()
            ? (files_[i].directory ? "directory" : "file")
            : !skills_.empty() ? skills_[i].description
                               : std::string(commands_[i]->desc);
        Element row                   = hbox({
            std::move(name) | xflex,
            text("  "),
            text(fit(description, description_width)) | dim
                | color(PANEL_FG_DIM),
        });
        row                           = row | xflex
            | (sel ? bgcolor(PANEL_COLOR_FOCUS) : bgcolor(PANEL_COLOR));
        rows.push_back(std::move(row));
    }
    const size_t remaining = total - first - shown;
    if (remaining > 0) {
        rows.push_back(text("  ↓ " + std::to_string(remaining) + " more") | dim
            | color(PANEL_FG_DIM));
    }
    return vbox(std::move(rows)) | xflex | bgcolor(PANEL_COLOR)
        | color(PANEL_FG);
}

} // namespace ursa
