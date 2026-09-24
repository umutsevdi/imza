#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/types.h"

namespace imza {

struct AttachmentToken {
    std::size_t begin = 0;
    std::size_t end   = 0;
    std::string query;
};

struct AttachmentCandidate {
    std::string path;
    bool directory = false;
};

struct AttachmentResult {
    Status status = Status::OK;
    std::optional<Attachment> attachment;
    std::string error;
};

std::optional<AttachmentToken> attachment_token_at(
    std::string_view text, std::size_t cursor);
bool can_add_attachment(const std::vector<Attachment>& existing,
    const Attachment& next, std::string& error);
std::vector<AttachmentCandidate> attachment_candidates(
    const std::filesystem::path& root, std::string_view query,
    std::size_t limit = 50);
AttachmentResult load_attachment(
    const std::filesystem::path& root, std::string_view relative_path);
std::string message_with_attachments(
    std::string_view text, const std::vector<Attachment>& attachments);
void retain_mentioned_attachments(
    std::string_view text, std::vector<Attachment>& attachments);

} // namespace imza
