#pragma once

#include <filesystem>
#include <string>
#include <variant>
#include <vector>

#include "common/tool_call.h"
#include "common/types.h"

namespace ursa {

struct QuestionCard {
    std::string prompt;
    std::vector<std::string> options;
    bool multi     = false;
    bool free_text = false;
};

using QuestionForm = std::vector<QuestionCard>;

struct QuestionAnswer {
    std::vector<std::string> selected;
    std::string free_text;
    std::string prompt { };
};

struct ModalAnswer {
    std::vector<QuestionAnswer> cards;
};

struct ConnectResult {
    std::string provider_id;
    std::string endpoint;
    std::string api_key;
    bool persist = true;
};

struct ModelChoice {
    std::string connection_id;
    std::string model_id;
};

struct VariantChoice {
    std::string effort;
};

struct SkillPolicyChange {
    std::string name;
    std::string project_root;
    SkillPolicy policy = SkillPolicy::ASK;
};

struct SkillPolicyChanges {
    std::vector<SkillPolicyChange> entries;
};

struct ConnectModal {
    enum class Entry { MANAGE, PICK_MODEL, SUBAGENTS };
    Entry entry = Entry::MANAGE;
};

struct ViewerModal {
    std::string title;
    std::string content;
    std::string lang;
    std::size_t start_line = 1;
    bool line_numbers      = true;
    std::string metadata;
};

struct VariantModal {
    std::vector<std::string> options;
    std::string current;
};

struct SessionsModal {
    std::vector<std::string> titles;
    std::vector<std::string> saved_at;
    std::vector<std::string> paths;
};

struct SkillsModal {
    struct Entry {
        std::string name;
        std::string description;
        std::string project_root;
        SkillPolicy policy = SkillPolicy::ASK;
    };
    std::vector<Entry> entries;
};

using ModalPayload = std::variant<std::monostate, ViewerModal, ToolCallRequest,
    QuestionForm, ConnectModal, VariantModal, SessionsModal, SkillsModal>;

using ModalResult
    = std::variant<std::monostate, ToolVerdict, ModalAnswer, ConnectResult,
        ModelChoice, VariantChoice, SkillPolicyChanges, std::filesystem::path>;

} // namespace ursa
