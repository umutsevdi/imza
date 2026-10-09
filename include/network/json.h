#pragma once

#include <glaze/glaze.hpp>
#include <glaze/json/json_t.hpp>

#include <optional>
#include <string>
#include <string_view>

#include "common/util.h"

// JSON policy: known wire shapes use reflected structs with the shared
// options below; glz::json_t (JsonValue) is reserved for genuinely
// dynamic payloads (Lua return values, model-supplied pass-through,
// vendor arrays that mix entry types).

namespace imza {

// Shared Glaze options: tolerate unknown fields (model-supplied args,
// forward-compatible persisted files, models.dev catalog growth) and omit
// empty optionals on write so files stay minimal.
inline constexpr glz::opts JSON_READ {
    .error_on_unknown_keys = false,
    .skip_null_members     = true,
};

inline constexpr glz::opts JSON_WRITE {
    .skip_null_members = true,
};

// Pretty variant of JSON_WRITE for persisted files, two-space indentation.
inline constexpr glz::opts JSON_WRITE_PRETTY {
    .skip_null_members = true,
    .prettify          = true,
    .indentation_width = 2,
};

// Glaze's writer emits raw NULs for control characters without a short
// escape, and tool output sliced on a byte boundary can end mid-character;
// both are invalid inside a JSON string literal. Dropping them here repairs
// reads and writes alike. Clean input passes through untouched.
inline std::optional<std::string> repair_json_string_bytes(
    std::string_view json)
{
    std::optional<std::string> repaired;
    std::size_t copied = 0; // bytes of `json` already appended to *repaired
    bool in_string     = false;
    bool escaped       = false;
    const auto drop    = [&](std::size_t i) {
        if (!repaired) {
            repaired.emplace(json.substr(0, i));
            copied = i;
        }
        repaired->append(json.substr(copied, i - copied));
        copied = i + 1;
    };
    for (std::size_t i = 0; i < json.size();) {
        const unsigned char c = static_cast<unsigned char>(json[i]);
        if (escaped) {
            escaped = false;
            ++i;
        } else if (in_string && c == '\\') {
            escaped = true;
            ++i;
        } else if (c == '"') {
            in_string = !in_string;
            ++i;
        } else if (in_string && c < 0x20) {
            drop(i);
            ++i;
        } else if (in_string && c >= 0x80) {
            const std::size_t length = utf8_valid_sequence_length(json, i);
            if (length == 0) {
                drop(i); // truncated or malformed sequence byte
            }
            i += length == 0 ? 1 : length;
        } else {
            ++i;
        }
    }
    if (repaired) {
        repaired->append(json.substr(copied));
    }
    return repaired;
}

// Parse into a reflected struct, reporting the parse error. The output
// is unspecified on failure.
template <typename T>
[[nodiscard]] glz::error_ctx json_parse_checked(std::string_view text, T& out)
{
    if (auto repaired = repair_json_string_bytes(text)) {
        return glz::read<JSON_READ>(out, *repaired);
    }
    return glz::read<JSON_READ>(out, text);
}

// Human-readable one-line diagnostic for a failed parse (byte offset and
// context), for logs and error messages.
[[nodiscard]] inline std::string json_parse_error(
    std::string_view text, const glz::error_ctx& error)
{
    return glz::format_error(error, text);
}

// Parse into a reflected struct, discarding the diagnostic.
template <typename T> bool json_parse(std::string_view text, T& out)
{
    return !json_parse_checked(text, out);
}

// Serialize a reflected struct, compact. Glaze writes keys in member
// order; an empty result is only possible on allocation failure, which
// callers treat as infallible. The checked variants surface the error
// where a caller actually can react (persisted-file writes).
template <typename T> std::string json_dump(const T& value)
{
    std::string out = glz::write<JSON_WRITE>(value).value_or(std::string { });
    if (auto repaired = repair_json_string_bytes(out)) {
        return std::move(*repaired);
    }
    return out;
}

template <typename T> std::string json_dump_pretty(const T& value)
{
    std::string out
        = glz::write<JSON_WRITE_PRETTY>(value).value_or(std::string { });
    if (auto repaired = repair_json_string_bytes(out)) {
        return std::move(*repaired);
    }
    return out;
}

template <typename T>
[[nodiscard]] std::optional<std::string> json_dump_checked(const T& value)
{
    auto out = glz::write<JSON_WRITE>(value);
    if (!out) {
        return std::nullopt;
    }
    if (auto repaired = repair_json_string_bytes(*out)) {
        return std::move(*repaired);
    }
    return std::move(out.value());
}

template <typename T>
[[nodiscard]] std::optional<std::string> json_dump_pretty_checked(
    const T& value)
{
    auto out = glz::write<JSON_WRITE_PRETTY>(value);
    if (!out) {
        return std::nullopt;
    }
    if (auto repaired = repair_json_string_bytes(*out)) {
        return std::move(*repaired);
    }
    return std::move(out.value());
}

// Serialize a range of reflected values as a compact JSON array string,
// mirroring json_dump's infallible convention: a failed item write
// degrades to `null`.
template <typename Range> std::string json_dump_array(const Range& items)
{
    std::string out = "[";
    bool first      = true;
    for (const auto& item : items) {
        if (!first) {
            out += ',';
        }
        first = false;
        out += json_dump(item);
    }
    out += ']';
    return out;
}

// Dynamic DOM for genuinely dynamic payloads: Lua return values, model-
// supplied tool args in display code, vendor error bodies, JWT claims.
using JsonValue = glz::json_t;

// Member lookup without json_t::operator[]'s auto-create side effect:
// nullptr when the value is not an object or the key is absent.
inline const JsonValue* find_member(
    const JsonValue& value, std::string_view key)
{
    if (!value.is_object()) {
        return nullptr;
    }
    const auto& object = value.get<JsonValue::object_t>();
    const auto found   = object.find(key);
    return found == object.end() ? nullptr : &found->second;
}

} // namespace imza