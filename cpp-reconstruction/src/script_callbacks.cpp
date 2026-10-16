#include <solace/script_callbacks.hpp>
#include <lua.hpp>
#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace solace::script {
namespace {
constexpr std::array<std::string_view, 6> queue_names{
    "presentQueue", "createMove", "frameStageNotify", "event", "input", "menuToggled"};
constexpr std::array<std::string_view, 25> event_names{
    "player_hurt", "player_death", "round_start", "round_end", "round_prestart",
    "bullet_impact", "weapon_fire", "weapon_reload", "weapon_zoom", "item_pickup",
    "player_spawn", "player_jump", "player_footstep", "bomb_beginplant", "bomb_planted",
    "bomb_begindefuse", "bomb_defused", "bomb_exploded", "hegrenade_detonate",
    "flashbang_detonate", "smokegrenade_detonate", "decoy_detonate",
    "inferno_startburn", "other_death", "player_falldamage"};
constexpr char queue_type[] = "lua_queue";
constexpr char anchor_type[] = "solace.callback_owner";
constexpr std::uint64_t userdata_magic = 0x534f4c4143454351ULL;
char anchor_key;

std::size_t queue_index(Queue queue) {
    const auto value = static_cast<std::size_t>(queue);
    if (value >= queue_names.size()) throw std::invalid_argument("invalid callback queue");
    return value;
}

struct Control {
    lua_State* state;
    std::uint32_t owner;
    ProtectedCall call;
    bool active{true};
    std::array<std::vector<int>, 6> references;

    void close() noexcept {
        if (!state) return;
        active = false;
        for (auto& queue : references) {
            for (const int reference : queue) luaL_unref(state, LUA_REGISTRYINDEX, reference);
            queue.clear();
        }
        state = nullptr;
    }
};

struct Userdata {
    std::uint64_t magic{userdata_magic};
    bool alive{true};
    Queue queue{};
    std::shared_ptr<Control> control;
};

Userdata* test_userdata(lua_State* state, int index, const char* type) {
    auto* data = static_cast<Userdata*>(luaL_testudata(state, index, type));
    if (!data || lua_rawlen(state, index) != sizeof(Userdata) || data->magic != userdata_magic)
        return nullptr;
    return data;
}

Userdata* check_queue(lua_State* state) {
    luaL_checkudata(state, 1, queue_type);
    auto* data = test_userdata(state, 1, queue_type);
    if (!data || static_cast<std::size_t>(data->queue) >= queue_names.size())
        luaL_argerror(state, 1, "invalid queue userdata");
    return data;
}

Control* current_owner(lua_State* state, Userdata* queue) {
    if (!queue->alive || !queue->control || !queue->control->state) return nullptr;
    auto* control = queue->control.get();
    lua_rawgetp(state, LUA_REGISTRYINDEX, &anchor_key);
    const auto* anchor = test_userdata(state, -1, anchor_type);
    const bool matches = anchor && anchor->alive && anchor->control.get() == control;
    lua_pop(state, 1);
    if (!matches) return nullptr;
    lua_getfield(state, LUA_REGISTRYINDEX, "__lua_script_id");
    int integer = 0;
    const auto owner = lua_tointegerx(state, -1, &integer);
    lua_pop(state, 1);
    return integer && owner == static_cast<lua_Integer>(control->owner) ? control : nullptr;
}

int add(lua_State* state) {
    auto* data = check_queue(state);
    luaL_checktype(state, 2, LUA_TFUNCTION);
    auto* owner = current_owner(state, data);
    if (!owner) return luaL_error(state, "the script has already been unloaded");
    lua_pushvalue(state, 2);
    const int reference = luaL_ref(state, LUA_REGISTRYINDEX);
    bool failed = false;
    try { owner->references[static_cast<std::size_t>(data->queue)].push_back(reference); }
    catch (...) { failed = true; }
    if (failed) {
        luaL_unref(state, LUA_REGISTRYINDEX, reference);
        return luaL_error(state, "callback reference allocation failed");
    }
    return 0;
}

int remove(lua_State* state) {
    auto* data = check_queue(state);
    luaL_checktype(state, 2, LUA_TFUNCTION);
    auto* owner = current_owner(state, data);
    if (!owner) return 0;
    auto& references = owner->references[static_cast<std::size_t>(data->queue)];
    for (auto item = references.begin(); item != references.end(); ++item) {
        lua_rawgeti(state, LUA_REGISTRYINDEX, *item);
        const bool equal = lua_rawequal(state, -1, 2) != 0;
        lua_pop(state, 1);
        if (equal) {
            luaL_unref(state, LUA_REGISTRYINDEX, *item);
            references.erase(item);
            break;
        }
    }
    return 0;
}

int tostring(lua_State* state) {
    check_queue(state);
    lua_pushliteral(state, "event queue");
    return 1;
}

template<bool Anchor> int collect(lua_State* state) {
    auto* data = test_userdata(state, 1, Anchor ? anchor_type : queue_type);
    if (data && data->alive) {
        data->alive = false;
        if constexpr (Anchor) {
            if (data->control) data->control->close();
        }
        std::destroy_at(&data->control);
    }
    return 0;
}

void push_userdata(lua_State* state, const std::shared_ptr<Control>& owner,
                   Queue queue, const char* type) {
    void* storage = lua_newuserdatauv(state, sizeof(Userdata), 0);
    std::construct_at(static_cast<Userdata*>(storage), Userdata{userdata_magic, true, queue, owner});
    luaL_setmetatable(state, type);
}
}

struct CallbackQueues::Impl { std::shared_ptr<Control> owner; };

CallbackQueues::CallbackQueues(lua_State* state, std::uint32_t owner_id, ProtectedCall call)
    : impl_(std::make_unique<Impl>()) {
    if (!state || !call) throw std::invalid_argument("callback state and protected call are required");
    const int top = lua_gettop(state);
    lua_rawgetp(state, LUA_REGISTRYINDEX, &anchor_key);
    const bool installed = !lua_isnil(state, -1);
    lua_pop(state, 1);
    if (installed) throw std::logic_error("callback queues already installed in this Lua state");
    lua_getfield(state, LUA_REGISTRYINDEX, "__lua_script_id");
    if (!lua_isnil(state, -1) &&
        (!lua_isinteger(state, -1) || lua_tointeger(state, -1) != static_cast<lua_Integer>(owner_id))) {
        lua_settop(state, top);
        throw std::invalid_argument("Lua registry owner ID does not match callback owner");
    }
    lua_pop(state, 1);
    luaL_getmetatable(state, queue_type);
    const bool conflict = !lua_isnil(state, -1);
    lua_pop(state, 1);
    if (conflict) throw std::logic_error("lua_queue metatable already exists");
    impl_->owner = std::make_shared<Control>(Control{state, owner_id, call, true, {}});
    lua_pushinteger(state, static_cast<lua_Integer>(owner_id));
    lua_setfield(state, LUA_REGISTRYINDEX, "__lua_script_id");

    luaL_newmetatable(state, anchor_type);
    lua_pushcfunction(state, collect<true>); lua_setfield(state, -2, "__gc");
    lua_pushliteral(state, "protected callback owner"); lua_setfield(state, -2, "__metatable");
    lua_pop(state, 1);
    push_userdata(state, impl_->owner, Queue::present, anchor_type);
    lua_rawsetp(state, LUA_REGISTRYINDEX, &anchor_key);

    luaL_newmetatable(state, queue_type);
    lua_newtable(state);
    const luaL_Reg methods[] = {{"Add", add}, {"Remove", remove}, {nullptr, nullptr}};
    luaL_setfuncs(state, methods, 0);
    lua_pushvalue(state, -1); lua_setfield(state, -3, "__index");
    lua_setfield(state, -2, "methods");
    lua_pushcfunction(state, tostring); lua_setfield(state, -2, "__tostring");
    lua_pushcfunction(state, collect<false>); lua_setfield(state, -2, "__gc");
    lua_pushliteral(state, "protected queue"); lua_setfield(state, -2, "__metatable");
    lua_pop(state, 1);
    lua_createtable(state, 0, static_cast<int>(queue_names.size()));
    for (std::size_t index = 0; index < queue_names.size(); ++index) {
        push_userdata(state, impl_->owner, static_cast<Queue>(index), queue_type);
        lua_setfield(state, -2, queue_names[index].data());
    }
    lua_setglobal(state, "events");
}

CallbackQueues::~CallbackQueues() { close(); }
bool CallbackQueues::active() const noexcept { return impl_->owner->state && impl_->owner->active; }
bool CallbackQueues::closed() const noexcept { return !impl_->owner->state; }
std::uint32_t CallbackQueues::owner_id() const noexcept { return impl_->owner->owner; }
std::size_t CallbackQueues::size(Queue queue) const { return impl_->owner->references[queue_index(queue)].size(); }
void CallbackQueues::deactivate() noexcept { impl_->owner->active = false; }
void CallbackQueues::close() noexcept { impl_->owner->close(); }

DispatchResult CallbackQueues::dispatch(Queue queue, int nargs) {
    const auto index = queue_index(queue);
    auto& owner = *impl_->owner;
    if (!owner.state) throw std::logic_error("callback owner is closed");
    lua_State* const state = owner.state;
    const int top = lua_gettop(state);
    if (nargs < 0 || nargs > top) throw std::invalid_argument("invalid callback argument count");
    if (nargs == std::numeric_limits<int>::max() || !lua_checkstack(state, nargs + 1))
        throw std::runtime_error("callback Lua stack capacity exhausted");
    const int base = top - nargs;
    struct StackRestore {
        lua_State* state;
        int base;
        ~StackRestore() { lua_settop(state, base); }
    } restore{state, base};
    DispatchResult result;
    // Copy integer registry references exactly as the recovered dispatchers do.
    const auto snapshot = owner.references[index];
    for (const int reference : snapshot) {
        if (!owner.active || !owner.state) break;
        const auto& live = owner.references[index];
        if (std::find(live.begin(), live.end(), reference) == live.end()) continue;
        lua_rawgeti(state, LUA_REGISTRYINDEX, reference);
        for (int argument = 1; argument <= nargs; ++argument) lua_pushvalue(state, base + argument);
        ++result.invoked;
        try { result.lua_status = owner.call(state, nargs, 0); }
        catch (...) { owner.active = false; throw; }
        if (result.lua_status != LUA_OK) {
            owner.active = false;
            const char* error = lua_tostring(state, -1);
            // Original protected-call reporting uses a C string (embedded NUL truncates).
            result.error = error ? error : "unknown error (non-string error value)";
            lua_settop(state, base);
            return result;
        }
    }
    lua_settop(state, base);
    return result;
}

std::string_view queue_name(Queue queue) { return queue_names[queue_index(queue)]; }
std::span<const std::string_view> registered_event_names() noexcept { return event_names; }
}
