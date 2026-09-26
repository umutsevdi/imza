#include "tools/lua.h"

#include "tools/bindings.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace imza {

LuaState::LuaState()  = default;
LuaState::~LuaState() = default;

void LuaState::register_module(const LuaModule& module)
{
    if (std::ranges::any_of(_modules, [&module](const LuaModule& existing) {
            return existing.name == module.name;
        })) {
        throw std::invalid_argument("LuaState: duplicate module");
    }
    _modules.push_back(module);
}

std::span<const LuaModule> LuaState::modules() const { return _modules; }

std::unique_ptr<LuaState> make_lua_state()
{
    auto state = std::make_unique<LuaState>();
    register_core(*state);
    register_fs(*state);
    register_web(*state);
    register_tree(*state);
    register_canvas(*state);
    return state;
}

} // namespace imza
