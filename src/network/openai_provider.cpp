#include "common/util.h"
#include "network/json.h"
#include "network/json_io.h"
#include "network/network.h"
#include "network/sse_parse.h"

namespace imza {

namespace openai {

    struct FunctionSpec {
        std::string name;
        std::string description;
        // Schema forwarded verbatim from the tool author.
        glz::raw_json parameters;
    };

    struct ToolWire {
        std::string type = "function";
        FunctionSpec function;
    };

    struct ImageUrl {
        std::string url;
    };

    struct FileContent {
        std::string filename;
        std::string file_data;
    };

    struct TextPart {
        std::string type = "text";
        std::string text;
    };

    struct MediaPart {
        std::string type;
        std::optional<ImageUrl> image_url;
        std::optional<FileContent> file;
    };

    using ContentPart = std::variant<TextPart, MediaPart>;

    struct CallFunction {
        std::string name;
        // Historical args cross back verbatim; OpenAI wants a string here.
        std::string arguments;
    };

    struct ToolCallWire {
        std::string id;
        std::string type = "function";
        CallFunction function;
    };

    // Message content is a string when plain, a part array when the user
    // message carries media; pre-serialized and embedded raw so the wire
    // keeps the single "content" key.
    struct RequestMessage {
        std::string role;
        glz::raw_json content;
        std::optional<std::vector<ToolCallWire>> tool_calls;
        std::optional<std::string> tool_call_id;
    };

    struct StreamOptions {
        bool include_usage = true;
    };

    struct RequestBody {
        std::string model;
        bool stream        = true;
        double temperature = 0.7;
        StreamOptions stream_options;
        std::optional<std::string> reasoning_effort;
        // OpenAI renamed the cap when reasoning is active.
        std::optional<std::uint64_t> max_completion_tokens;
        std::optional<std::uint64_t> max_tokens;
        std::optional<std::vector<ToolWire>> tools;
        std::optional<std::string> tool_choice;
        std::vector<RequestMessage> messages;
    };

    std::string build(const ChatRequest& req)
    {
        RequestBody body;
        body.model       = req.model;
        body.stream      = true;
        body.temperature = req.temperature;
        if (req.reasoning_effort) {
            body.reasoning_effort      = *req.reasoning_effort;
            body.max_completion_tokens = req.max_output_tokens;
        } else {
            body.max_tokens = req.max_output_tokens;
        }
        if (!req.tools.empty()) {
            std::vector<ToolWire> tools;
            tools.reserve(req.tools.size());
            for (const auto& t : req.tools) {
                tools.push_back({ "function",
                    { t.name, t.description,
                        glz::raw_json { t.parameters } } });
            }
            body.tools       = std::move(tools);
            body.tool_choice = "auto";
        }

        for (const auto& m : req.messages) {
            RequestMessage message;
            message.role = role_str(m.type);
            if (m.type == Message::Type::USER && !m.media.empty()) {
                std::vector<ContentPart> parts;
                if (!m.content.empty()) {
                    parts.push_back(TextPart { "text", m.content });
                }
                for (const Attachment& media : m.media) {
                    if (media.type == Attachment::Type::IMAGE) {
                        parts.push_back(MediaPart { "image_url",
                            ImageUrl { media_data_url(media) }, std::nullopt });
                    } else {
                        parts.push_back(MediaPart { "file", std::nullopt,
                            FileContent {
                                media.path, media_data_url(media) } });
                    }
                }
                message.content = glz::raw_json { json_dump_array(parts) };
            } else {
                message.content = glz::raw_json(
                    glz::write<JSON_WRITE>(m.content).value_or("null"));
            }
            if (!m.tool_calls.empty()) {
                std::vector<ToolCallWire> calls;
                calls.reserve(m.tool_calls.size());
                for (const auto& tc : m.tool_calls) {
                    calls.push_back(
                        { tc.id, "function", { tc.name, tc.args } });
                }
                message.tool_calls = std::move(calls);
            }
            if (m.type == Message::Type::TOOL) {
                message.tool_call_id = m.tool_call_id;
            }
            body.messages.push_back(std::move(message));
        }
        return json_dump(body);
    }

    struct ToolCallDelta {
        std::optional<int> index;
        std::optional<std::string> id;
        struct Function {
            std::optional<std::string> name;
            std::optional<std::string> arguments;
        };
        std::optional<Function> function;
    };

    struct ChunkDelta {
        std::optional<std::string> content;
        std::optional<std::string> reasoning;
        std::optional<std::string> reasoning_content;
        std::optional<std::vector<ToolCallDelta>> tool_calls;
    };

    struct UsageDetails {
        std::uint64_t cached_tokens = 0;
    };

    struct UsageWire {
        std::uint64_t prompt_tokens     = 0;
        std::uint64_t completion_tokens = 0;
        std::uint64_t total_tokens      = 0;
        std::optional<UsageDetails> prompt_tokens_details;
    };

    struct Choice {
        std::optional<ChunkDelta> delta;
        std::optional<std::string> finish_reason;
        std::optional<UsageWire> usage;
    };

    struct Chunk {
        std::optional<UsageWire> usage;
        std::vector<Choice> choices;
        // Error payloads carry an error object instead of choices.
        struct Error {
            std::optional<std::string> message;
        };
        std::optional<Error> error;
    };

    Usage to_usage(const UsageWire& u)
    {
        Usage out;
        out.prompt     = u.prompt_tokens;
        out.completion = u.completion_tokens;
        out.total      = u.total_tokens;
        if (u.prompt_tokens_details) {
            out.cached_read = u.prompt_tokens_details->cached_tokens;
        }
        return out;
    }

    void take_delta(const ToolCallDelta& tc, ParseState& state,
        std::vector<StreamEvent>& outs)
    {
        ToolAccum& acc = state.tool_accums[tc.index.value_or(0)];
        if (tc.id) {
            acc.id = *tc.id;
        }
        if (tc.function) {
            if (tc.function->name && !tc.function->name->empty()) {
                acc.name = *tc.function->name;
            }
            if (tc.function->arguments) {
                acc.args += *tc.function->arguments;
            }
        }
        emit_ready_tool_start(acc, outs);
    }

    void parse(ParseState& state, std::string_view, std::string_view data,
        std::vector<StreamEvent>& outs)
    {
        if (data == "[DONE]") {
            state.terminal = true;
            flush_tool_accums(state, outs);
            outs.push_back(make_done_event());
            return;
        }
        Chunk chunk;
        if (json_parse_checked(data, chunk)) {
            outs.push_back(make_error_event(Status::JSON_ERROR));
            return;
        }
        if (chunk.choices.empty() && chunk.error) {
            std::string msg;
            Status status = parse_api_error(data, msg);
            if (status == Status::OK) {
                status = Status::API_ERROR;
            }
            outs.push_back(
                make_error_event(status, chunk.error->message.value_or("")));
            return;
        }
        std::optional<Usage> u;
        if (chunk.usage) {
            u = to_usage(*chunk.usage);
        }
        bool done = false;
        if (!chunk.choices.empty()) {
            const Choice& first = chunk.choices.front();
            if (first.delta) {
                const ChunkDelta& delta = *first.delta;
                if (delta.content) {
                    outs.push_back(make_delta_event(*delta.content));
                }
                if (delta.reasoning && !delta.reasoning->empty()) {
                    outs.push_back(make_reasoning_event(*delta.reasoning));
                } else if (delta.reasoning_content
                    && !delta.reasoning_content->empty()) {
                    outs.push_back(
                        make_reasoning_event(*delta.reasoning_content));
                }
                if (delta.tool_calls) {
                    for (const auto& tc : *delta.tool_calls) {
                        take_delta(tc, state, outs);
                    }
                }
            }
            if (first.finish_reason) {
                flush_tool_accums(state, outs);
                done = true;
            }
            if (first.usage) {
                u = to_usage(*first.usage);
            }
        }
        if (u) {
            emit_usage_once(state, *u, outs);
        } else {
            emit_usage_once(state, Usage { }, outs);
        }
        if (done) {
            outs.push_back(make_done_event());
        } else if (outs.empty()) {
            outs.push_back(make_delta_event(""));
        }
    }

} // namespace openai

extern const Provider openai_provider = { openai::build, openai::parse };

} // namespace imza
