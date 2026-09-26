#include "ui/autocomplete.h"

#include "app/application_state.h"
#include "common/util.h"
#include "providers/store.h"
#include "tools/skills.h"
#include "ui/ui.h"
#include "workspace/environment.h"

#include <algorithm>
#include <filesystem>

namespace imza {

Autocomplete::Set Autocomplete::active_set() const
{
    if (!_files.empty()) {
        return Set::FILES;
    }
    if (!_skills.empty()) {
        return Set::SKILLS;
    }
    return Set::COMMANDS;
}

int Autocomplete::count() const
{
    switch (active_set()) {
    case Set::FILES: return static_cast<int>(_files.size());
    case Set::SKILLS: return static_cast<int>(_skills.size());
    case Set::COMMANDS: break;
    }
    return static_cast<int>(_commands.size());
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
    _token               = attachment_token_at(text, at);
    if (_token) {
        _files = attachment_candidates(
            std::filesystem::current_path(), _token->query);
        return;
    }
    const std::size_t begin = word_begin(text, at);
    if (begin < at && text[begin] == '$' && mention_end(text, begin) >= at) {
        _skill_begin = begin;
        const std::string key
            = to_lower(text.substr(begin + 1, at - begin - 1));
        const std::vector<Skill> allowed = allowed_skills(
            state.environment->skills(), state.providers->config());
        for (const Skill& skill : allowed) {
            const std::string name = to_lower(skill.name);
            if (name.starts_with(key)) {
                _skills.push_back(skill);
            }
        }
        return;
    }
    _skill_begin.reset();
    if (text.empty() || text[0] != '/' || text.find(' ') != std::string::npos) {
        return;
    }
    const std::string key = to_lower(text);
    for (const auto& c : slash_commands()) {
        const std::string name = to_lower(c.name);
        if (name.size() >= key.size()
            && name.compare(0, key.size(), key) == 0) {
            _commands.push_back(&c);
        }
    }
    if (_commands.size() == 1 && _commands[0]->name == text) {
        _commands.clear();
    }
}

bool Autocomplete::accept(const ApplicationState& state, std::string& text,
    int& cursor, std::vector<Attachment>& attachments)
{
    if (!_files.empty() && _token) {
        const AttachmentCandidate& candidate
            = _files[static_cast<std::size_t>(selected_)];
        const std::string replacement = "@" + candidate.path;
        text.replace(_token->begin, _token->end - _token->begin, replacement);
        cursor = static_cast<int>(_token->begin + replacement.size());
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
            attachments.end(), [&](const Attachment& attachment) {
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
    if (!_skills.empty() && _skill_begin) {
        const std::string replacement
            = "$" + _skills[static_cast<std::size_t>(selected_)].name;
        text.replace(*_skill_begin,
            static_cast<std::size_t>(cursor) - *_skill_begin, replacement);
        cursor = static_cast<int>(*_skill_begin + replacement.size());
        text.insert(static_cast<std::size_t>(cursor), " ");
        ++cursor;
        refresh(state, text, cursor);
        return false;
    }
    const SlashCommand* cmd = _commands[static_cast<std::size_t>(selected_)];
    text                    = cmd->name;
    cursor                  = static_cast<int>(text.size());
    refresh(state, text, cursor);
    return true;
}

void Autocomplete::clear()
{
    _commands.clear();
    _skills.clear();
    _files.clear();
    _token.reset();
    _skill_begin.reset();
    selected_ = 0;
}

ftxui::Element Autocomplete::render(const LayoutCtx& ctx) const
{
    using namespace ftxui;
    const size_t max_rows       = 8;
    const int available_width   = ctx.kind == LayoutCtx::Kind::WIDE
        ? ctx.width - LayoutCtx::PANEL_WIDTH
        : ctx.width;
    const int description_width = std::clamp(available_width - 28, 8, 56);
    const Set set               = active_set();
    const size_t total          = set == Set::FILES ? _files.size()
        : set == Set::SKILLS                        ? _skills.size()
                                                    : _commands.size();
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
        const std::string name_text = set == Set::FILES ? "@" + _files[i].path
            : set == Set::SKILLS ? "$" + _skills[i].name
                                 : std::string(_commands[i]->name);
        Element name                = text(name_text);
        if (sel) {
            name = name | bold;
        }
        const std::string description = set == Set::FILES
            ? (_files[i].directory ? "directory" : "file")
            : set == Set::SKILLS ? _skills[i].description
                                 : std::string(_commands[i]->desc);
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

} // namespace imza
