#include <doctest/doctest.h>

#include <string>

#include "network/json_io.h"
#include "tools/bindings.h"
#include "tools/lua.h"
#include "tools/tool.h"

namespace imza {

namespace {

    // Runs a script through a fresh trusted-mode lua tool; returns output.
    std::string eval_script(const std::string& script)
    {
        auto state      = make_lua_state();
        const Tool tool = make_lua_tool(*state);
        Json::Value args(Json::objectValue);
        args["script"] = script;
        return tool.run({ "lua", "", "", "" }, args).text;
    }

} // namespace

TEST_CASE("binding catalog registers every path as a callable function")
{
    const std::string script = R"lua(
local paths = {
  "fs.read", "fs.list", "fs.grep",
  "todo.get", "todo.set", "ask",
  "shell",
  "web.fetch", "web.search",
  "fs.insert", "fs.edit", "fs.write",
  "tree.index", "tree.nodes", "tree.symbols", "tree.references",
  "canvas.line", "canvas.bar", "canvas.pie", "canvas.surface",
}
for _, path in ipairs(paths) do
  local t = imza
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
    CHECK(eval_script("print(type(imza.shell))") == "function\n");
    CHECK(eval_script("local o, e = imza.web.fetch('https://example.invalid') "
                      "print(e)")
        == "web.fetch: web access is disabled for this run\n");
    CHECK(eval_script("local o, e = imza.shell('echo hi') print(e)")
        == "shell: shell access is disabled for this run\n");
}

TEST_CASE("core description embeds autoload docs, lists others by name")
{
    auto state                     = make_lua_state();
    const Tool tool                = make_lua_tool(*state);
    const std::string& description = tool.spec.description;

    // Autoload modules appear with their methods; non-autoload ones only
    // as a name: description line inside the <modules> block, derived
    // from the live catalog rather than copied literals.
    for (const LuaModule& module : state->modules()) {
        const std::string header
            = std::string(module.name).append(": ").append(module.description);
        if (module.autoload) {
            CHECK(description.find(module.name) != std::string::npos);
            for (const LuaMethod& method : module.methods) {
                if (method.is_private) {
                    continue;
                }
                std::string path = std::string(method.name);
                if (path.find('.') == std::string::npos
                    && !module.name.empty()) {
                    path = std::string(module.name) + "." + path;
                }
                path = "imza." + path;
                CHECK_MESSAGE(
                    description.find(path + "(") != std::string::npos, path);
            }
        } else {
            const std::size_t at = description.find(header);
            REQUIRE(at != std::string::npos);
            CHECK(description.find("<modules>") < at);
            for (const LuaMethod& method : module.methods) {
                std::string path = std::string(method.name);
                if (path.find('.') == std::string::npos
                    && !module.name.empty()) {
                    path = std::string(module.name) + "." + path;
                }
                path = "imza." + path;
                CHECK(description.find(path) == std::string::npos);
            }
        }
    }
    CHECK(description.find("imza.<name>(args...)") != std::string::npos);
}

TEST_CASE("load returns tree documentation while bindings are always present")
{
    auto state      = make_lua_state();
    const Tool lua  = make_lua_tool(*state);
    const Tool load = make_load_tool(*state);
    Json::Value script(Json::objectValue);
    script["script"]          = "return type(imza.tree.index)";
    const ToolOutput callable = lua.run({ "lua", "", "", "" }, script);
    CHECK(callable.return_value->asString() == "function");

    const ToolOutput documentation = load.run(
        { "load", "", "", "" }, parse_json(R"json({"name":"tree"})json"));
    CHECK(documentation.kind == ToolOutput::Kind::OUTPUT);
    CHECK(documentation.text.find("TYPES") != std::string::npos);
    CHECK(documentation.text.find("METHODS") != std::string::npos);
    for (const LuaMethod& method : tree_lua_methods()) {
        if (method.is_private) {
            continue;
        }
        CHECK(documentation.text.find(
                  std::string("imza.tree.") + std::string(method.name) + "(")
            != std::string::npos);
    }

    const ToolOutput canvas = load.run(
        { "load", "", "", "" }, parse_json(R"json({"name":"canvas"})json"));
    CHECK(canvas.kind == ToolOutput::Kind::OUTPUT);
    for (const LuaMethod& method : canvas_lua_methods()) {
        CHECK(canvas.text.find(
                  std::string("imza.canvas.") + std::string(method.name) + "(")
            != std::string::npos);
    }

    const ToolOutput unknown = load.run(
        { "load", "", "", "" }, parse_json(R"json({"name":"unknown"})json"));
    CHECK(unknown.kind == ToolOutput::Kind::ERROR);
    CHECK(unknown.text.find("load: unknown module") != std::string::npos);
}

} // namespace imza
