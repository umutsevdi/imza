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

} // namespace

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
