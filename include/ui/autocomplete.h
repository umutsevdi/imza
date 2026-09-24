#pragma once

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <app/slash_commands.h>
#include <tools/skills.h>
#include <ui/ui.h>
#include <workspace/attachments.h>

#include <optional>
#include <string>
#include <vector>

namespace imza {

struct ApplicationState;

// Suggestion popup for the chat input: slash commands, $skill mentions and
// @file attachments.
class Autocomplete {
public:
    bool active() const;

    bool handle_event(const ftxui::Event& event);
    void refresh(
        const ApplicationState& state, const std::string& text, int cursor);

    // Applies the highlighted suggestion. Returns true when a command was
    // chosen and the caller should submit the input afterwards.
    bool accept(const ApplicationState& state, std::string& text, int& cursor,
        std::vector<Attachment>& attachments);

    void clear();
    ftxui::Element render(const LayoutCtx& ctx) const;

private:
    int count() const;

    std::vector<const SlashCommand*> _commands;
    std::vector<Skill> _skills;
    std::vector<AttachmentCandidate> _files;
    std::optional<AttachmentToken> _token;
    std::optional<std::size_t> _skill_begin;
    int selected_ = 0;
};

} // namespace imza
