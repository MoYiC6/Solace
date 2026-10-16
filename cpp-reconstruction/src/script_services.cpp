#include <solace/script_services.hpp>
#include <lua.hpp>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cmath>
#include <climits>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace solace::script {
namespace {
struct Execution {
    unsigned timeout;
    ULONGLONG deadline;
    Execution* previous;
};
thread_local Execution* execution = nullptr;
void timeout_hook(lua_State* state, lua_Debug*) {
    if (execution && GetTickCount64() > execution->deadline)
        luaL_error(state, "script timed out (took longer than %d ms - infinite loop?)",
                   static_cast<int>(execution->timeout));
}
struct HookScope {
    lua_State* state;
    lua_Hook old_hook;
    int old_mask, old_count;
    Execution current;
    HookScope(lua_State* lua, unsigned timeout)
        : state(lua), old_hook(lua_gethook(lua)), old_mask(lua_gethookmask(lua)),
          old_count(lua_gethookcount(lua)),
          current{execution ? execution->timeout : timeout,
                  execution ? execution->deadline : GetTickCount64() + timeout, execution} {
        execution = &current;
        lua_sethook(state, timeout_hook, LUA_MASKCOUNT, 100000);
    }
    ~HookScope() {
        lua_sethook(state, old_hook, old_mask, old_count);
        execution = current.previous;
    }
};
template<int (*Function)(lua_State*)> int guarded(lua_State* state) {
    try { return Function(state); }
    catch (const std::exception& error) { lua_pushstring(state, error.what()); }
    catch (...) { lua_pushliteral(state, "script service: unknown C++ error"); }
    return lua_error(state);
}
void valid_time(float seconds) {
    if (!std::isfinite(seconds)) throw std::invalid_argument("Script host time must be finite");
}
}

int protected_call_with_timeout(lua_State* state, int arguments, int results, unsigned timeout_ms) {
    if (!state || timeout_ms == 0 || timeout_ms > static_cast<unsigned>(INT_MAX))
        throw std::invalid_argument("Invalid Lua call or timeout");
    HookScope scope(state, timeout_ms);
    return lua_pcall(state, arguments, results, 0);
}
int protected_call(lua_State* state, int arguments, int results) {
    return protected_call_with_timeout(state, arguments, results, 2000);
}

struct Services::Impl {
    struct Task { float due; int reference; };
    struct Anchor { Impl* owner; };
    static constexpr const char* metatable = "solace.recovered.services";
    lua_State* state;
    int anchor_reference = LUA_NOREF;
    float time;
    bool closed = false;
    bool dispatching = false;
    std::vector<Task> tasks;
    std::vector<std::string> logs;

    Impl(lua_State* lua, float seconds) : state(lua), time(seconds) {
        valid_time(time);
        const int top = lua_gettop(state);
        auto* anchor = static_cast<Anchor*>(lua_newuserdatauv(state, sizeof(Anchor), 0));
        anchor->owner = this;
        luaL_newmetatable(state, metatable);
        lua_pushliteral(state, "script services"); lua_setfield(state, -2, "__metatable");
        lua_setmetatable(state, -2);
        lua_pushvalue(state, -1);
        anchor_reference = luaL_ref(state, LUA_REGISTRYINDEX);
        lua_pushvalue(state, -1); lua_pushcclosure(state, guarded<print>, 1); lua_setglobal(state, "print");
        lua_pushvalue(state, -1); lua_pushcclosure(state, guarded<delay>, 1); lua_setglobal(state, "Delay");
        lua_getglobal(state, "table"); lua_getfield(state, -1, "unpack"); lua_setglobal(state, "unpack");
        lua_settop(state, top);
        // error() already is the official Lua 5.4 base implementation recovered
        // at 0x18028A700; reuse it instead of writing a second implementation.
    }
    static Impl& owner(lua_State* state) {
        const int index = lua_upvalueindex(1);
        auto* anchor = static_cast<Anchor*>(luaL_testudata(state, index, metatable));
        if (!anchor || lua_rawlen(state, index) != sizeof(Anchor) || !anchor->owner || anchor->owner->closed)
            throw std::runtime_error("Script services are closed");
        return *anchor->owner;
    }
    static int print(lua_State* state) {
        const int count = lua_gettop(state);
        if (count > INT_MAX - 3 || !lua_checkstack(state, count + 3))
            return luaL_error(state, "print: insufficient Lua stack");
        // __tostring may raise. Convert every argument before constructing a
        // C++ string, so its destructor is never bypassed by that Lua error.
        for (int i = 1; i <= count; ++i) luaL_tolstring(state, i, nullptr);
        auto& self = owner(state);
        std::string line;
        for (int i = 1; i <= count; ++i) {
            if (i > 1) line.push_back(' ');
            line.append(lua_tostring(state, count + i)); // Original strlen truncates NUL.
        }
        self.logs.push_back(std::move(line));
        lua_settop(state, count);
        return 0;
    }
    static int delay(lua_State* state) {
        float seconds = static_cast<float>(luaL_checknumber(state, 1));
        luaL_checktype(state, 2, LUA_TFUNCTION);
        auto& self = owner(state);
        if (!std::isfinite(seconds)) seconds = 0.0f;
        const float due = std::fmax(0.0f, seconds) + self.time;
        lua_pushvalue(state, 2);
        const int reference = luaL_ref(state, LUA_REGISTRYINDEX);
        try { self.tasks.push_back({due, reference}); }
        catch (...) { luaL_unref(state, LUA_REGISTRYINDEX, reference); throw; }
        return 0;
    }
    void clear_tasks() noexcept {
        for (const auto& task : tasks) luaL_unref(state, LUA_REGISTRYINDEX, task.reference);
        tasks.clear();
    }
    void close() noexcept {
        if (closed) return;
        closed = true;
        clear_tasks();
        lua_rawgeti(state, LUA_REGISTRYINDEX, anchor_reference);
        auto* anchor = static_cast<Anchor*>(luaL_testudata(state, -1, metatable));
        if (anchor && lua_rawlen(state, -1) == sizeof(Anchor)) anchor->owner = nullptr;
        lua_pop(state, 1);
        luaL_unref(state, LUA_REGISTRYINDEX, anchor_reference);
        anchor_reference = LUA_NOREF;
    }
};

Services::Services(lua_State* state, float time) {
    if (!state) throw std::invalid_argument("Services require a Lua state");
    impl_ = std::make_unique<Impl>(state, time);
}
Services::~Services() { close(); }
void Services::close() noexcept { if (impl_) impl_->close(); }
void Services::set_time(float seconds) {
    valid_time(seconds);
    if (impl_->closed) throw std::runtime_error("Script services are closed");
    impl_->time = seconds;
}
std::size_t Services::pending() const noexcept { return impl_->tasks.size(); }
std::vector<std::string> Services::take_logs() { return std::exchange(impl_->logs, {}); }
std::size_t Services::dispatch_due(unsigned timeout_ms) {
    if (timeout_ms == 0 || timeout_ms > static_cast<unsigned>(INT_MAX))
        throw std::invalid_argument("Lua timeout must be a positive signed millisecond value");
    auto& self = *impl_;
    if (self.closed) throw std::runtime_error("Script services are closed");
    if (self.dispatching) throw std::runtime_error("Recursive delayed-task dispatch");
    std::vector<Impl::Task> due, pending;
    due.reserve(self.tasks.size()); pending.reserve(self.tasks.size());
    for (const auto& task : self.tasks)
        (task.due > self.time ? pending : due).push_back(task);
    self.tasks.swap(pending);
    struct Guard {
        Impl& self;
        std::vector<Impl::Task>& due;
        int top;
        ~Guard() {
            for (const auto& task : due)
                if (task.reference != LUA_NOREF) luaL_unref(self.state, LUA_REGISTRYINDEX, task.reference);
            lua_settop(self.state, top);
            self.dispatching = false;
        }
    } guard{self, due, lua_gettop(self.state)};
    self.dispatching = true;
    std::size_t invoked = 0;
    for (auto& task : due) {
        if (self.closed) break;
        lua_rawgeti(self.state, LUA_REGISTRYINDEX, task.reference);
        luaL_unref(self.state, LUA_REGISTRYINDEX, task.reference);
        task.reference = LUA_NOREF;
        ++invoked;
        const int status = protected_call_with_timeout(self.state, 0, 0, timeout_ms);
        if (status != LUA_OK) {
            const char* error = lua_tostring(self.state, -1);
            self.clear_tasks();
            throw std::runtime_error(error ? error : "unknown error (non-string error value)");
        }
    }
    return invoked;
}
} // namespace solace::script
