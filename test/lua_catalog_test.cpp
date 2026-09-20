#include <doctest/doctest.h>

#include <string>

#include "network/json_io.h"
#include "tools/tool.h"

namespace imza {

namespace {

    // Runs a script through a fresh trusted-mode lua tool; returns output.
    std::string eval_script(const std::string& script)
    {
        const Tool tool = make_lua_tool();
        Json::Value args(Json::objectValue);
        args["script"] = script;
        return tool.run(args).text;
    }

    // The exact description shipped before the binding catalog existed,
    // used to prove the generated text is byte-identical.
    constexpr std::string_view LEGACY_DESCRIPTION
        = R"desc(Executes a sandboxed Lua script and returns printed content
and modified files.
Base libraries: string, table, math, coroutine (io/os/package are absent).

TYPES
  FileEntry  = { path: string, type: "file" | "dir", size?: string }  -- "4.2 KB"
  TodoStatus = "pending" | "in_progress" | "completed" | "cancelled"
  TodoItem   = { content: string, status: TodoStatus }
  AskCard    = { prompt: string, options?: string[], multi?: bool, free_text?: bool }
  AskAnswer  = { question: string, answer: string }
  GrepHit    = { file: string, line: integer, text: string }

LEGEND
  tool.<name>(args...) => Value | (nil, Err)
  - Err is a string
  - `?` optional with its default after `=`.
  - An ungranted path returns nil, Err. Check the second return value

METHODS
tool.read(path: string, first_line?: integer=1, last_line?: integer=nil)
    => string
    Read the file at `path` returning its content.
    first_line..last_line inclusive omit last_line to read to the end.
    Fails on no such file, first_line past the end, last_line < first_line, or a
    binary file. Over 64 KB is cut and marked "[truncated]".

tool.list(path?: string=".", depth?: integer=1, show_hidden?: bool=false)
    => FileEntry[]
    List files and directories in `path`.
    Filename-sorted listing; depth (1..5) descends into subdirectories and
    their entries come back flat, so join child names to their parent
    yourself. `size` is absent for directories and "-" when unreadable.
    Capped at 2000 entries.

tool.grep(path: string, pattern: string) => GrepHit[]
    Run a POSIX extended regex (not a Lua pattern) over a file or
    directory tree, one hit per matching line.
    Capped at 500 hits, followed by a hit whose text is "[truncated]".

tool.todo.get() => TodoItem[]
    The session task list in display order; empty array when unset.

tool.todo.set(items: TodoItem[]) => true
    Set todo items.
    Replaces the entire list: get, modify, set the full array back.
    `status` defaults to "pending"; any other value is rejected.

tool.ask(cards: AskCard[]) => AskAnswer[]
    Puts a question to the end user and returns their answer.
    `options` offers a choice list, `multi` allows several picks, `free_text` allows
    typed input; a card may combine them, and `answer` is the typed text
    plus the selected labels joined with ", ". nil, Err when dismissed.
    Unavailable in unattended runs.

tool.shell(command: string, timeout?: integer=10, workspace?: string)
    => output: string, exit_code: integer
    Runs a single external command, returning its captured output (capped at
    64 KB) and exit status.
    A non-zero exit_code is a successful call, so test exit_code rather than nil.
    nil, Err means it could not start, timed out (1..120 s) or was denied.
    Chains and pipelines are rejected. Compose results in Lua instead.
    `workspace` is the directory the command runs in.

tool.web.fetch(url: string) => string
    Fetches an http(s) URL as readable text: HTML is converted to plain
    text, other bodies (JSON, markdown, raw) return as-is.
    Fails on non-http(s) URLs, network errors, non-2xx responses, bodies over 5 MB,
    and pages with no readable content. Capped at 40000 characters.

tool.web.search(query: string, num_results?: integer=5) => string
    Search results as a formatted text block.
    num_results is clamped to 1..10. No hits returns "No search results found.
    Try a different query."

tool.file.insert(path: string, text: string, line?: integer=nil) => true
    Inserts text before the 1-based line, pushing it down; omit line to
    append at the end. A line past the end of the file is an error.

tool.file.edit(path: string, old: string, new: string, count?: integer=1) => true
    Replaces the first count occurrences of old with new; count=0 replaces
    all. old is an exact literal match, so include enough surrounding text
    to be unique. Errors if old is empty or not found.

tool.file.write(path: string, text: string) => true
    Replaces the file's entire content, creating it if absent. Prefer
    insert/edit for targeted changes; this discards everything else.)desc";

} // namespace

TEST_CASE("generated lua description matches the historical literal")
{
    const Tool tool = make_lua_tool();
    CHECK(tool.spec.description == LEGACY_DESCRIPTION);
}

TEST_CASE("binding catalog registers every path as a callable function")
{
    const std::string script = R"lua(
local paths = {
  "read", "list", "grep",
  "todo.get", "todo.set", "ask",
  "shell",
  "web.fetch", "web.search",
  "file.insert", "file.edit", "file.write",
}
for _, path in ipairs(paths) do
  local t = tool
  local name = path
  local dot = path:find("%.")
  if dot then
    t = t[path:sub(1, dot - 1)]
    name = path:sub(dot + 1)
  end
  if type(t) ~= "table" then error("missing table for " .. path) end
  if type(t[name]) ~= "function" then error("missing function " .. path) end
end
print("catalog-ok")
)lua";
    CHECK(eval_script(script) == "catalog-ok\n");
}

TEST_CASE("capability-disabled bindings fail closed, not absent")
{
    // Default host: web/shell capabilities are off, but the bindings stay
    // registered and return nil, err rather than vanishing from the table.
    CHECK(eval_script("print(type(tool.shell))") == "function\n");
    CHECK(eval_script("local o, e = tool.web.fetch('https://example.invalid') "
                      "print(e)")
        == "web.fetch: web access is disabled for this run\n");
    CHECK(eval_script("local o, e = tool.shell('echo hi') print(e)")
        == "shell: shell access is disabled for this run\n");
}

TEST_CASE("every catalog path appears in the generated description")
{
    const Tool tool = make_lua_tool();
    for (const std::string_view path : { "tool.read", "tool.list", "tool.grep",
             "tool.todo.get", "tool.todo.set", "tool.ask", "tool.shell",
             "tool.web.fetch", "tool.web.search", "tool.file.insert",
             "tool.file.edit", "tool.file.write" }) {
        CHECK(tool.spec.description.find(path) != std::string::npos);
    }
}

} // namespace imza
