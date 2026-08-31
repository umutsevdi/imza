#pragma once

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <agent/attachments.h>
#include <agent/slash_commands.h>
#include <environment/skills.h>
#include <ui/ui.h>

#include <optional>
#include <string>
#include <vector>

namespace ursa {

class ApplicationState;

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
        std::vector<FileAttachment>& attachments);

    void clear();
    ftxui::Element render(const LayoutCtx& ctx) const;

private:
    int count() const;

    std::vector<const SlashCommand*> commands_;
    std::vector<Skill> skills_;
    std::vector<AttachmentCandidate> files_;
    std::optional<AttachmentToken> token_;
    std::optional<std::size_t> skill_begin_;
    int selected_ = 0;
};

} // namespace ursa
