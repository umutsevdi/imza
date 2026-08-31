#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "types.h"

namespace ursa {

struct FileAttachment {
    std::string path;
    std::string content;
};

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
    std::optional<FileAttachment> attachment;
    std::string error;
};

std::optional<AttachmentToken> attachment_token_at(
    std::string_view text, std::size_t cursor);
std::vector<AttachmentCandidate> attachment_candidates(
    const std::filesystem::path& root, std::string_view query,
    std::size_t limit = 50);
AttachmentResult load_attachment(
    const std::filesystem::path& root, std::string_view relative_path);
std::string message_with_attachments(
    std::string_view text, const std::vector<FileAttachment>& attachments);
void retain_mentioned_attachments(
    std::string_view text, std::vector<FileAttachment>& attachments);

} // namespace ursa
