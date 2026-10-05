#pragma once

#include <glaze/glaze.hpp>
#include <glaze/json/json_t.hpp>

#include <optional>
#include <string>
#include <string_view>

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

// Parse into a reflected struct, reporting the parse error. The output
// is unspecified on failure.
template <typename T>
[[nodiscard]] glz::error_ctx json_parse_checked(std::string_view text, T& out)
{
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
    return glz::write<JSON_WRITE>(value).value_or(std::string { });
}

template <typename T> std::string json_dump_pretty(const T& value)
{
    return glz::write<JSON_WRITE_PRETTY>(value).value_or(std::string { });
}

template <typename T>
[[nodiscard]] std::optional<std::string> json_dump_checked(const T& value)
{
    auto out = glz::write<JSON_WRITE>(value);
    if (!out) {
        return std::nullopt;
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
        out += glz::write<JSON_WRITE>(item).value_or("null");
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