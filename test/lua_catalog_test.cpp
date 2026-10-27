#include <doctest/doctest.h>

#include <string>

#include "network/json.h"
#include "test_helpers.h"
#include "tools/bindings.h"
#include "tools/lua.h"
#include "tools/tool.h"

namespace imza {

namespace {

    std::string eval_script(const std::string& script)
    {
        return imza::test::run_lua(script).text;
    }

} // namespace

TEST_CASE("binding catalog registers every path as a callable function")
{
    // Derived from the live module registry, so a new binding is covered
    // without editing a literal path list.
    auto state = make_lua_state();
    REQUIRE_FALSE(state->modules().empty());
    std::string script = "local missing = {}\n";
    for (const LuaModule& module : state->modules()) {
        for (const LuaMethod& method : module.methods) {
            std::string path = std::string(method.name);
            if (!module.name.empty()) {
                path = std::string(module.name) + "." + path;
            }
            script += "do local t, name = imza, '" + path + "'\n";
            script += "local dot = name:find('%.')\n";
            script += "if dot then t = t[name:sub(1, dot - 1)];"
                      " name = name:sub(dot + 1) end\n";
            script += "if type(t) ~= 'table' or type(t[name]) ~= 'function'"
                      " then missing[#missing + 1] = name end end\n";
        }
    }
    script += "print(#missing == 0 and 'catalog-ok' or "
              "table.concat(missing, ','))\n";
    CHECK(eval_script(script).find("catalog-ok") != std::string::npos);
}

TEST_CASE("capability-disabled bindings fail closed, not absent")
{
    // Default host: web/shell capabilities are off, but the bindings stay
    // registered and return nil, err rather than vanishing from the table.
    CHECK(eval_script("print(type(imza.shell))").find("function")
        != std::string::npos);
    const std::string fetch = eval_script(
        "print(select(2, pcall(imza.web.fetch, 'https://example.invalid')))");
    CHECK(fetch.find("web.fetch: web access is disabled") != std::string::npos);
    const std::string shell
        = eval_script("print(select(2, pcall(imza.shell, 'echo hi')))");
    CHECK(shell.find("shell: shell access is disabled") != std::string::npos);
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
    // The fatal-binding contract ships in the LEGEND: failures throw and
    // abort; no nil, err convention or => signatures are promised anywhere.
    CHECK(description.find("returns Value, throws") != std::string::npos);
    CHECK(description.find("pcall") != std::string::npos);
    CHECK(description.find("(nil, Err") == std::string::npos);
    CHECK(description.find("if err then") == std::string::npos);
    CHECK(description.find("=> ") == std::string::npos);
    CHECK(description.find("(raises)") == std::string::npos);
}

TEST_CASE("load returns module documentation while bindings are always present")
{
    auto state      = make_lua_state();
    const Tool load = make_load_tool(*state);

    const ToolOutput canvas
        = load.run({ "load_module", R"json({"name":"canvas"})json", "", "" });
    CHECK(canvas.kind == ToolOutput::Kind::OUTPUT);
    for (const LuaMethod& method : canvas_module().methods) {
        CHECK(canvas.text.find(
                  std::string("imza.canvas.") + std::string(method.name) + "(")
            != std::string::npos);
    }
    const ToolOutput plan
        = load.run({ "load_module", R"json({"name":"plan"})json", "", "" });
    CHECK(plan.kind == ToolOutput::Kind::OUTPUT);
    CHECK(plan.text.find("imza.plan.create(") != std::string::npos);
    CHECK(plan.text.find("imza.plan.edit(") != std::string::npos);
    CHECK(plan.text.find("# Requirements") != std::string::npos);
    CHECK(plan.text.find("# Verification") != std::string::npos);
    CHECK(plan.text.find("since your last get()") != std::string::npos);
    CHECK(plan.text.find("APPLYING PINNED NOTES") != std::string::npos);
    CHECK(plan.text.find("frozen") != std::string::npos);
    CHECK(plan.text.find("ready for build") == std::string::npos);

    const ToolOutput unknown
        = load.run({ "load_module", R"json({"name":"unknown"})json", "", "" });
    CHECK(unknown.kind == ToolOutput::Kind::ERROR);
    CHECK(
        unknown.text.find("load_module: unknown module") != std::string::npos);
}

} // namespace imza
