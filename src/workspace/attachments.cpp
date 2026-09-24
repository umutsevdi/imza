#include "workspace/attachments.h"
#include "common/util.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace imza {

namespace {

    constexpr std::uintmax_t MAX_ATTACHMENT_BYTES = 1024 * 1024;
    constexpr std::size_t MAX_ATTACHMENTS         = 20;
    constexpr std::size_t MAX_TOTAL_BYTES         = 4 * 1024 * 1024;

    bool ignored_directory(std::string_view name)
    {
        static const std::unordered_set<std::string> ignored { ".git", ".hg",
            ".svn", ".cache", ".next", ".nuxt", ".idea", ".vscode", ".venv",
            "venv", "node_modules", "build", "dist", "out", "target", "vendor",
            "coverage", "__pycache__" };
        return ignored.contains(std::string(name));
    }

    std::optional<std::pair<Attachment::Type, std::string>> native_type(
        std::string_view content)
    {
        const auto starts_with = [&](std::string_view signature) {
            return content.size() >= signature.size()
                && content.compare(0, signature.size(), signature) == 0;
        };
        if (starts_with(std::string_view("\x89PNG\r\n\x1a\n", 8))) {
            return std::pair { Attachment::Type::IMAGE, "image/png" };
        }
        if (starts_with(std::string_view("\xff\xd8\xff", 3))) {
            return std::pair { Attachment::Type::IMAGE, "image/jpeg" };
        }
        if (starts_with("GIF87a") || starts_with("GIF89a")) {
            return std::pair { Attachment::Type::IMAGE, "image/gif" };
        }
        if (content.size() >= 12 && content.compare(0, 4, "RIFF") == 0
            && content.compare(8, 4, "WEBP") == 0) {
            return std::pair { Attachment::Type::IMAGE, "image/webp" };
        }
        if (starts_with("%PDF-")) {
            return std::pair { Attachment::Type::PDF, "application/pdf" };
        }
        return std::nullopt;
    }

    std::string escape_attribute(std::string_view value)
    {
        std::string out;
        for (const char c : value) {
            if (c == '&') {
                out += "&amp;";
            } else if (c == '"') {
                out += "&quot;";
            } else if (c == '<') {
                out += "&lt;";
            } else if (c == '>') {
                out += "&gt;";
            } else {
                out += c;
            }
        }
        return out;
    }

} // namespace

std::optional<AttachmentToken> attachment_token_at(
    std::string_view text, std::size_t cursor)
{
    cursor                  = std::min(cursor, text.size());
    const std::size_t begin = word_begin(text, cursor);
    if (begin >= cursor || text[begin] != '@') {
        return std::nullopt;
    }
    const std::string_view token = text.substr(begin + 1, cursor - begin - 1);
    if (token.find('@') != std::string_view::npos) {
        return std::nullopt;
    }
    return AttachmentToken { begin, cursor, std::string(token) };
}

bool can_add_attachment(const std::vector<Attachment>& existing,
    const Attachment& next, std::string& error)
{
    std::size_t total
        = next.type == Attachment::Type::TEXT ? next.content.size() : 0;
    for (const auto& attachment : existing) {
        if (attachment.type == Attachment::Type::TEXT) {
            total += attachment.content.size();
        }
    }
    if (existing.size() >= MAX_ATTACHMENTS || total > MAX_TOTAL_BYTES) {
        error = "Attachments exceed the 20-file or 4 MiB total text limit.";
        return false;
    }
    return true;
}

std::vector<AttachmentCandidate> attachment_candidates(
    const std::filesystem::path& root, std::string_view query,
    std::size_t limit)
{
    std::filesystem::path typed     = path_from_utf8(query);
    std::filesystem::path directory = typed.parent_path();
    const std::string needle        = to_lower(typed.filename().string());
    std::error_code ec;
    const auto canonical_root = std::filesystem::weakly_canonical(root, ec);
    if (ec) {
        return { };
    }
    const auto search_dir
        = std::filesystem::weakly_canonical(canonical_root / directory, ec);
    if (ec || !path_within(canonical_root, search_dir)) {
        return { };
    }
    std::vector<AttachmentCandidate> out;
    std::filesystem::directory_iterator it(search_dir,
        std::filesystem::directory_options::skip_permission_denied, ec);
    constexpr std::size_t MAX_INSPECTED = 2000;
    std::size_t inspected               = 0;
    for (const auto& entry : it) {
        if (ec || inspected++ >= MAX_INSPECTED) {
            break;
        }
        const std::string name = entry.path().filename().string();
        if (!needle.empty()
            && to_lower(name).find(needle) == std::string::npos) {
            continue;
        }
        const bool is_dir = entry.is_directory(ec) && !entry.is_symlink(ec);
        if (is_dir && ignored_directory(name)) {
            continue;
        }
        if (!is_dir && !entry.is_regular_file(ec)) {
            continue;
        }
        std::string path = utf8_from_path(directory / name);
        if (is_dir) {
            path += '/';
        }
        out.push_back({ std::move(path), is_dir });
        if (out.size() >= limit) {
            break;
        }
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        if (a.directory != b.directory) {
            return a.directory > b.directory;
        }
        return a.path < b.path;
    });
    return out;
}

AttachmentResult load_attachment(
    const std::filesystem::path& root, std::string_view relative_path)
{
    std::error_code ec;
    const auto canonical_root = std::filesystem::weakly_canonical(root, ec);
    const auto path           = std::filesystem::weakly_canonical(
        canonical_root / path_from_utf8(relative_path), ec);
    if (ec || !path_within(canonical_root, path)) {
        return { Status::CONFIG_ERROR, std::nullopt,
            "Attachment must be inside the workspace." };
    }
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        return { Status::CONFIG_ERROR, std::nullopt,
            "Attachment is not a readable file: " + std::string(relative_path)
                + "." };
    }
    std::ifstream file(path, std::ios::binary);
    char signature[16];
    file.read(signature, sizeof signature);
    const auto native = native_type(
        std::string_view(signature, static_cast<std::size_t>(file.gcount())));
    if (!native) {
        const std::uintmax_t size = std::filesystem::file_size(path, ec);
        if (ec || size > MAX_ATTACHMENT_BYTES) {
            return { Status::CONFIG_ERROR, std::nullopt,
                "Attachment exceeds the 1 MiB limit: "
                    + std::string(relative_path) + "." };
        }
    }
    file.clear();
    file.seekg(0);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    if (!file && !file.eof()) {
        return { Status::CONFIG_ERROR, std::nullopt,
            "Could not read attachment: " + std::string(relative_path) + "." };
    }
    std::string content   = buffer.str();
    Attachment::Type type = Attachment::Type::TEXT;
    std::string media_type;
    if (native) {
        type       = native->first;
        media_type = native->second;
    } else if (content.find('\0') != std::string::npos) {
        return { Status::CONFIG_ERROR, std::nullopt,
            "Unsupported attachment: Imza supports text, images, and PDFs: "
                + std::string(relative_path) + "." };
    }
    const std::string display
        = utf8_from_path(std::filesystem::relative(path, canonical_root, ec));
    return { Status::OK,
        Attachment { display, std::move(content), type, std::move(media_type) },
        "" };
}

std::string message_with_attachments(
    std::string_view text, const std::vector<Attachment>& attachments)
{
    const bool has_text = std::any_of(attachments.begin(), attachments.end(),
        [](const Attachment& attachment) {
            return attachment.type == Attachment::Type::TEXT;
        });
    if (!has_text) {
        return std::string(text);
    }
    std::string out(text);
    out += "\n\n<attachments>\n";
    for (const auto& attachment : attachments) {
        if (attachment.type != Attachment::Type::TEXT) {
            continue;
        }
        out += "<file path=\"" + escape_attribute(attachment.path) + "\">\n";
        out += attachment.content;
        if (!attachment.content.empty() && attachment.content.back() != '\n') {
            out += '\n';
        }
        out += "</file>\n";
    }
    out += "</attachments>";
    return out;
}

void retain_mentioned_attachments(
    std::string_view text, std::vector<Attachment>& attachments)
{
    std::erase_if(attachments, [&](const Attachment& attachment) {
        const std::string mention = "@" + attachment.path;
        std::size_t pos           = text.find(mention);
        while (pos != std::string_view::npos) {
            const std::size_t end   = pos + mention.size();
            const bool begins_token = pos == 0
                || std::isspace(static_cast<unsigned char>(text[pos - 1]));
            const bool ends_token = end == text.size()
                || std::isspace(static_cast<unsigned char>(text[end]));
            if (begins_token && ends_token) {
                return false;
            }
            pos = text.find(mention, pos + 1);
        }
        return true;
    });
}

} // namespace imza
