#include "common/util.h"
#include "network/json.h"
#include "network/network.h"
#include "network/sse_parse.h"

namespace imza {

namespace {

    // Wire structs: Glaze writes members in declaration order; optionals
    // are omitted when empty (JSON_WRITE skips null members).
    struct TextBlock {
        std::string type = "text";
        std::string text;
    };

    struct ThinkingBlockWire {
        std::string type = "thinking";
        std::string thinking;
        std::string signature;
    };

    struct SourceBlock {
        std::string type = "base64";
        std::string media_type;
        std::string data;
    };

    struct MediaBlock {
        std::string type;
        SourceBlock source;
    };

    struct ToolUseBlock {
        std::string type = "tool_use";
        std::string id;
        std::string name;
        // Historical args cross back to the provider verbatim; raw_json
        // avoids a parse/serialize round-trip per tool call per request.
        glz::raw_json input;
    };

    struct ToolResultBlock {
        std::string type = "tool_result";
        std::string tool_use_id;
        std::string content;
    };

    using ContentBlock = std::variant<TextBlock, ThinkingBlockWire, MediaBlock,
        ToolUseBlock, ToolResultBlock>;

    // Message content is a string when plain text, a block array when the
    // message carries thinking/media/tool calls; pre-serialized and
    // embedded raw so the wire keeps the single "content" key.
    struct RequestMessage {
        std::string role;
        glz::raw_json content;
    };

    RequestMessage string_message(std::string role, std::string content)
    {
        // A plain JSON string serializes with quotes/escapes already; use
        // it as the raw content.
        auto quoted = glz::write<JSON_WRITE>(content);
        return { std::move(role), glz::raw_json(quoted.value_or("null")) };
    }

    struct ToolSpecWire {
        std::string name;
        std::string description;
        // Schema forwarded verbatim from the tool author.
        glz::raw_json input_schema;
    };

    struct ThinkingConfig {
        std::string type            = "enabled";
        std::uint64_t budget_tokens = 0;
    };

    struct RequestBody {
        std::string model;
        bool stream              = true;
        double temperature       = 0.7;
        std::uint64_t max_tokens = 0;
        std::optional<ThinkingConfig> thinking;
        std::optional<std::vector<std::string>> system;
        std::vector<RequestMessage> messages;
        std::optional<std::vector<ToolSpecWire>> tools;
    };

    RequestMessage plain_message(std::string role, std::string content)
    {
        return string_message(std::move(role), std::move(content));
    }

    std::string build(const ChatRequest& req)
    {
        RequestBody body;
        body.model       = req.model;
        body.stream      = true;
        body.temperature = req.temperature;
        const std::uint64_t output_tokens
            = req.max_output_tokens.value_or(4096);
        body.max_tokens = req.thinking_budget
            ? *req.thinking_budget + output_tokens
            : output_tokens;
        if (req.thinking_budget) {
            body.thinking = ThinkingConfig { "enabled", *req.thinking_budget };
        }

        std::vector<std::string> system;
        std::vector<ToolResultBlock> tool_results;
        auto flush_results = [&]() {
            if (!tool_results.empty()) {
                RequestMessage message = plain_message("user", "");
                message.content
                    = glz::raw_json { json_dump_array(tool_results) };
                body.messages.push_back(std::move(message));
                tool_results.clear();
            }
        };

        for (const auto& m : req.messages) {
            if (m.type == Message::Type::SYSTEM) {
                system.push_back(m.content);
                continue;
            }
            if (m.type == Message::Type::TOOL) {
                tool_results.push_back(
                    { "tool_result", m.tool_call_id, m.content });
                continue;
            }
            flush_results();
            const std::string role = role_str(m.type);
            if (!m.tool_calls.empty() || !m.thinking.empty()
                || (m.type == Message::Type::USER && !m.media.empty())) {
                std::vector<ContentBlock> blocks;
                for (const auto& tb : m.thinking) {
                    blocks.push_back(ThinkingBlockWire {
                        "thinking", tb.text, tb.signature });
                }
                if (!m.content.empty()) {
                    blocks.push_back(TextBlock { "text", m.content });
                }
                if (m.type == Message::Type::USER) {
                    for (const Attachment& media : m.media) {
                        blocks.push_back(MediaBlock {
                            media.type == Attachment::Type::IMAGE ? "image"
                                                                  : "document",
                            { "base64", media.media_type,
                                base64_encode(media.content) } });
                    }
                }
                for (const auto& tc : m.tool_calls) {
                    blocks.push_back(ToolUseBlock { "tool_use", tc.id, tc.name,
                        glz::raw_json { tc.args } });
                }
                // Serialize the block array once and embed it raw; content
                // is either a quoted string or an array on the wire.
                RequestMessage message;
                message.role    = role;
                message.content = glz::raw_json { json_dump_array(blocks) };
                body.messages.push_back(std::move(message));
            } else {
                body.messages.push_back(plain_message(role, m.content));
            }
        }
        flush_results();
        if (!system.empty()) {
            body.system = std::move(system);
        }
        if (!req.tools.empty()) {
            std::vector<ToolSpecWire> tools;
            tools.reserve(req.tools.size());
            for (const auto& t : req.tools) {
                tools.push_back(
                    { t.name, t.description, glz::raw_json { t.parameters } });
            }
            body.tools = std::move(tools);
        }
        auto out = glz::write<JSON_WRITE>(body);
        return out ? std::move(out.value()) : std::string { };
    }

    // --- SSE parsing: typed payload views -------------------------------
    // Only the members the parse switch consumes; optionals tolerate
    // provider drift and absent fields default harmlessly.

    struct ContentBlockStart {
        struct Block {
            std::string type;
            std::string id;
            std::string name;
            std::optional<std::string> thinking;
        };
        Block content_block;
        int index = 0;
    };

    struct MessageStart {
        struct Usage {
            std::uint64_t input_tokens                = 0;
            std::uint64_t cache_read_input_tokens     = 0;
            std::uint64_t cache_creation_input_tokens = 0;
        };
        struct Message {
            std::optional<Usage> usage;
        };
        Message message;
    };

    struct MessageDelta {
        struct Usage {
            std::uint64_t output_tokens = 0;
        };
        std::optional<Usage> usage;
    };

    struct BlockDelta {
        struct Delta {
            std::string type;
            std::string text;
            std::string thinking;
            std::string signature;
            std::string partial_json;
        };
        Delta delta;
        int index = 0;
    };

    struct BlockStop {
        int index = 0;
    };

    void parse(ParseState& state, std::string_view event, std::string_view data,
        std::vector<StreamEvent>& outs)
    {
        if (event == "content_block_start") {
            ContentBlockStart payload;
            if (json_parse_checked(data, payload)) {
                return;
            }
            if (payload.content_block.type == "tool_use") {
                ToolAccum& acc = state.tool_accums[payload.index];
                acc.id         = payload.content_block.id;
                acc.name       = payload.content_block.name;
                emit_ready_tool_start(acc, outs);
            } else if (payload.content_block.type == "thinking") {
                state.thinking_accums[payload.index].text
                    = payload.content_block.thinking.value_or("");
            }
            return;
        }
        if (event == "message_start") {
            MessageStart payload;
            if (json_parse_checked(data, payload)) {
                return;
            }
            if (payload.message.usage) {
                const auto& usage        = *payload.message.usage;
                state.usage.cached_read  = usage.cache_read_input_tokens;
                state.usage.cached_write = usage.cache_creation_input_tokens;
                state.usage.prompt       = usage.input_tokens
                    + usage.cache_read_input_tokens
                    + usage.cache_creation_input_tokens;
                state.usage.total = state.usage.prompt;
            }
            return;
        }
        if (event == "message_delta") {
            MessageDelta payload;
            if (json_parse_checked(data, payload)) {
                return;
            }
            if (payload.usage) {
                state.usage.completion = payload.usage->output_tokens;
                state.usage.total = state.usage.prompt + state.usage.completion;
            }
            return;
        }
        if (event == "content_block_delta") {
            BlockDelta payload;
            if (json_parse_checked(data, payload)) {
                return;
            }
            const auto& delta = payload.delta;
            if (delta.type == "text_delta") {
                outs.push_back(make_delta_event(delta.text));
            } else if (delta.type == "input_json_delta") {
                auto it = state.tool_accums.find(payload.index);
                if (it != state.tool_accums.end()) {
                    it->second.args += delta.partial_json;
                }
            } else if (delta.type == "thinking_delta") {
                ThinkingAccum& acc = state.thinking_accums[payload.index];
                acc.text += delta.thinking;
                outs.push_back(make_reasoning_event(delta.thinking));
            } else if (delta.type == "signature_delta") {
                ThinkingAccum& acc = state.thinking_accums[payload.index];
                acc.signature += delta.signature;
            }
            return;
        }
        if (event == "content_block_stop") {
            BlockStop payload;
            if (json_parse_checked(data, payload)) {
                return;
            }
            auto it = state.tool_accums.find(payload.index);
            if (it != state.tool_accums.end()) {
                const ToolCallRequest req = finish_accum(it->second);
                state.tool_accums.erase(it);
                outs.push_back(make_tool_call_event(req));
                return;
            }
            auto think_it = state.thinking_accums.find(payload.index);
            if (think_it != state.thinking_accums.end()) {
                outs.push_back(
                    make_reasoning_event("", think_it->second.signature));
                state.thinking_accums.erase(think_it);
            }
            return;
        }
        if (event == "message_stop") {
            state.terminal = true;
            flush_tool_accums(state, outs);
            emit_usage_once(state, state.usage, outs);
            outs.push_back(make_done_event());
            return;
        }
    }

} // namespace

extern const Provider anthropic_provider = { build, parse };

} // namespace imza
