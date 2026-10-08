#include "common/util.h"
#include "network/json.h"
#include "network/json_io.h"
#include "network/network.h"
#include "network/sse_parse.h"

namespace imza {

namespace openai_responses {

    struct ToolWire {
        std::string type = "function";
        std::string name;
        std::string description;
        // Schema forwarded verbatim from the tool author.
        glz::raw_json parameters;
    };

    struct CallOutput {
        std::string type = "function_call_output";
        std::string call_id;
        std::string output;
    };

    struct SummaryEntry {
        std::string type = "summary_text";
        std::string text;
    };

    struct ReasoningItem {
        std::string type = "reasoning";
        std::string encrypted_content;
        std::vector<SummaryEntry> summary;
    };

    struct TextPart {
        std::string type = "input_text";
        std::string text;
    };

    struct MediaPart {
        std::string type;
        std::optional<std::string> image_url;
        std::optional<std::string> filename;
        std::optional<std::string> file_data;
    };

    using InputContent = std::variant<TextPart, MediaPart>;

    // Message content is a string when plain, typed parts when media is
    // attached; pre-serialized and embedded raw for the single "content" key.
    struct InputMessage {
        std::string role;
        glz::raw_json content;
    };

    struct CallItem {
        std::string type = "function_call";
        std::string call_id;
        std::string name;
        // Historical args cross back verbatim; the API wants a string.
        std::string arguments;
    };

    using InputItem
        = std::variant<CallOutput, ReasoningItem, InputMessage, CallItem>;

    struct ReasoningConfig {
        std::string effort;
        std::string summary = "auto";
    };

    struct RequestBody {
        std::string model;
        bool stream = true;
        bool store  = false;
        std::vector<std::string> include { "reasoning.encrypted_content" };
        std::optional<ReasoningConfig> reasoning;
        std::optional<std::uint64_t> max_output_tokens;
        std::optional<std::vector<ToolWire>> tools;
        std::optional<std::string> tool_choice;
        std::vector<InputItem> input;
    };

    void append_message(std::vector<InputItem>& input, const Message& message)
    {
        if (message.type == Message::Type::TOOL) {
            input.push_back(CallOutput { "function_call_output",
                message.tool_call_id, message.content });
            return;
        }

        for (const ThinkingBlock& thinking : message.thinking) {
            if (thinking.signature.empty()) {
                continue;
            }
            ReasoningItem item { "reasoning", thinking.signature, { } };
            if (!thinking.text.empty()) {
                item.summary.push_back(
                    SummaryEntry { "summary_text", thinking.text });
            }
            input.push_back(std::move(item));
        }

        if (!message.content.empty() || message.tool_calls.empty()
            || (message.type == Message::Type::USER
                && !message.media.empty())) {
            InputMessage item;
            item.role = role_str(message.type);
            if (message.type == Message::Type::USER && !message.media.empty()) {
                std::vector<InputContent> parts;
                if (!message.content.empty()) {
                    parts.push_back(TextPart { "input_text", message.content });
                }
                for (const Attachment& media : message.media) {
                    if (media.type == Attachment::Type::IMAGE) {
                        parts.push_back(
                            MediaPart { "input_image", media_data_url(media),
                                std::nullopt, std::nullopt });
                    } else {
                        parts.push_back(MediaPart { "input_file", std::nullopt,
                            media.path, media_data_url(media) });
                    }
                }
                item.content = glz::raw_json { json_dump_array(parts) };
            } else {
                item.content = glz::raw_json(
                    glz::write<JSON_WRITE>(message.content).value_or("null"));
            }
            input.push_back(std::move(item));
        }
        for (const ToolCallEntry& call : message.tool_calls) {
            input.push_back(
                CallItem { "function_call", call.id, call.name, call.args });
        }
    }

    std::string build(const ChatRequest& req)
    {
        RequestBody body;
        body.model  = req.model;
        body.stream = true;
        body.store  = false;
        if (req.reasoning_effort) {
            body.reasoning = ReasoningConfig { *req.reasoning_effort, "auto" };
        }
        body.max_output_tokens = req.max_output_tokens;
        if (!req.tools.empty()) {
            std::vector<ToolWire> tools;
            tools.reserve(req.tools.size());
            for (const auto& tool : req.tools) {
                tools.push_back({ "function", tool.name, tool.description,
                    glz::raw_json { tool.parameters } });
            }
            body.tools       = std::move(tools);
            body.tool_choice = "auto";
        }
        for (const Message& message : req.messages) {
            append_message(body.input, message);
        }
        return json_dump(body);
    }

    struct UsageDetails {
        std::uint64_t cached_tokens      = 0;
        std::uint64_t cache_write_tokens = 0;
    };

    struct UsageWire {
        std::uint64_t input_tokens  = 0;
        std::uint64_t output_tokens = 0;
        std::uint64_t total_tokens  = 0;
        std::optional<UsageDetails> input_tokens_details;
    };

    Usage to_usage(const UsageWire& wire)
    {
        Usage usage;
        usage.prompt     = wire.input_tokens;
        usage.completion = wire.output_tokens;
        usage.total      = wire.total_tokens;
        if (wire.input_tokens_details) {
            usage.cached_read  = wire.input_tokens_details->cached_tokens;
            usage.cached_write = wire.input_tokens_details->cache_write_tokens;
        }
        return usage;
    }

    struct ResponseItem {
        std::string type;
        std::optional<std::string> call_id;
        std::optional<std::string> id;
        std::optional<std::string> name;
        std::optional<std::string> arguments;
        std::optional<std::string> encrypted_content;
    };

    // Event envelope: every event carries type/delta/output_index; only
    // the completion/failed family carries response.
    struct Event {
        std::string type;
        std::optional<std::string> delta;
        std::optional<int> output_index;
        std::optional<ResponseItem> item;
        std::optional<UsageWire> usage;
        struct Response {
            std::optional<UsageWire> usage;
            struct Error {
                std::optional<std::string> message;
            };
            std::optional<Error> error;
        };
        std::optional<Response> response;
        std::optional<std::string> message;
    };

    Usage read_usage(const Event::Response& response)
    {
        if (response.usage) {
            return to_usage(*response.usage);
        }
        return Usage { };
    }

    void finish_response(ParseState& state, const Event::Response& response,
        std::vector<StreamEvent>& outs)
    {
        emit_usage_once(state, read_usage(response), outs);
        state.terminal = true;
        flush_tool_accums(state, outs);
        outs.push_back(make_done_event());
    }

    void parse(ParseState& state, std::string_view event, std::string_view data,
        std::vector<StreamEvent>& outs)
    {
        Event ev;
        if (json_parse_checked(data, ev)) {
            outs.push_back(make_error_event(Status::JSON_ERROR));
            return;
        }
        std::string type = ev.type.empty() ? std::string(event) : ev.type;
        if (type == "response.output_text.delta"
            || type == "response.refusal.delta") {
            outs.push_back(make_delta_event(ev.delta.value_or("")));
            return;
        }
        if (type == "response.reasoning_summary_text.delta"
            || type == "response.reasoning_text.delta") {
            outs.push_back(make_reasoning_event(ev.delta.value_or("")));
            return;
        }
        if (type == "response.output_item.added") {
            if (ev.item && ev.item->type == "function_call") {
                ToolAccum& acc = state.tool_accums[ev.output_index.value_or(0)];
                acc.id   = ev.item->call_id.value_or(ev.item->id.value_or(""));
                acc.name = ev.item->name.value_or("");
                acc.args = ev.item->arguments.value_or("");
                emit_ready_tool_start(acc, outs);
            }
            return;
        }
        if (type == "response.function_call_arguments.delta") {
            state.tool_accums[ev.output_index.value_or(0)].args
                += ev.delta.value_or("");
            return;
        }
        if (type == "response.output_item.done") {
            const int index = ev.output_index.value_or(0);
            if (ev.item && ev.item->type == "function_call") {
                ToolAccum& acc = state.tool_accums[index];
                if (ev.item->call_id && !ev.item->call_id->empty()) {
                    acc.id = *ev.item->call_id;
                }
                if (ev.item->name && !ev.item->name->empty()) {
                    acc.name = *ev.item->name;
                }
                if (ev.item->arguments) {
                    acc.args = *ev.item->arguments;
                }
                outs.push_back(make_tool_call_event(finish_accum(acc)));
                state.tool_accums.erase(index);
            } else if (ev.item && ev.item->type == "reasoning") {
                if (ev.item->encrypted_content
                    && !ev.item->encrypted_content->empty()) {
                    outs.push_back(
                        make_reasoning_event("", *ev.item->encrypted_content));
                }
            }
            return;
        }
        if (type == "response.completed" || type == "response.incomplete") {
            if (ev.response) {
                finish_response(state, *ev.response, outs);
            } else {
                // Completed payloads nest usage under response; a bare
                // usage field is tolerated for robustness.
                Event::Response bare;
                bare.usage = std::move(ev.usage);
                finish_response(state, bare, outs);
            }
            return;
        }
        if (type == "response.failed" || type == "error") {
            state.terminal = true;
            std::string message;
            if (type == "response.failed") {
                if (ev.response && ev.response->error
                    && ev.response->error->message) {
                    message = *ev.response->error->message;
                }
            } else if (ev.message) {
                message = *ev.message;
            }
            outs.push_back(make_error_event(Status::API_ERROR, message));
        }
    }

} // namespace openai_responses

extern const Provider openai_responses_provider
    = { openai_responses::build, openai_responses::parse };

} // namespace imza
