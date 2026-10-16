#include <solace/script.hpp>
#include <solace/script_callbacks.hpp>
#include <solace/script_services.hpp>
#include <lua.hpp>
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace solace::script;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
int main() {
    try {
        Runtime runtime({}, RuntimeOptions{17, 25, 10.0f});
        require(lua_gettop(runtime.state()) == 0, "registration leaked the stack");
        runtime.run(R"(
            assert(unpack == table.unpack)
            assert(debug.getregistry().__lua_script_id == 17)
            print('hello', 3, false, nil)
            print('a\0b', 'c')
            print()
            assert(not pcall(print, setmetatable({}, {__tostring=function() error('bad tostring') end})))
            print('after error')
            hits = {}
            assert(select('#', Delay(1, function() hits[#hits+1] = 'one'; Delay(0, function() hits[#hits+1]='nested' end) end)) == 0)
            Delay(0, function() hits[#hits+1] = 'zero' end)
            Delay(-1, function() hits[#hits+1] = 'negative' end)
            Delay(0/0, function() hits[#hits+1] = 'nan' end)
            Delay(math.huge, function() hits[#hits+1] = 'infinity' end)
            Delay(1e40, function() hits[#hits+1] = 'float overflow' end)
            assert(not pcall(Delay, 'invalid', function() end))
            assert(not pcall(Delay, 1, {}))
        )");
        require(runtime.take_logs() == std::vector<std::string>{"hello 3 false nil", "a c", "", "after error"}, "print contract mismatch");
        require(runtime.pending_tasks() == 6, "delayed count mismatch");
        require(runtime.advance_time(9) == 0, "early dispatch");
        require(runtime.advance_time(10) == 5, "invalid/negative delay normalization mismatch");
        require(runtime.advance_time(11) == 1 && runtime.pending_tasks() == 1, "nested delay ran in the same batch");
        require(runtime.advance_time(11) == 1 && runtime.pending_tasks() == 0, "nested delay not released");
        runtime.run("assert(table.concat(hits, ',') == 'zero,negative,nan,infinity,float overflow,one,nested')");
        runtime.run("Delay(0,function() error('delayed failure') end); Delay(0,function() error('must not run') end); Delay(100,function() end)");
        bool failed = false;
        try { runtime.advance_time(11); }
        catch (const std::runtime_error& error) { failed = std::string(error.what()).find("delayed failure") != std::string::npos; }
        require(failed && runtime.pending_tasks() == 0 && lua_gettop(runtime.state()) == 0, "failed delay leaked refs/stack or hid error");
        for (const char* code : {"while true do end", "assert(coroutine.resume(coroutine.create(function() while true do end end)))"}) {
            const auto start = std::chrono::steady_clock::now();
            failed = false;
            try { runtime.run(code); }
            catch (const std::runtime_error& error) { failed = std::string(error.what()).find("script timed out") != std::string::npos; }
            require(failed, "infinite loop did not time out");
            require(std::chrono::steady_clock::now() - start < std::chrono::seconds(3), "timeout took too long");
            require(lua_gettop(runtime.state()) == 0, "timeout leaked Lua stack");
        }
        runtime.run("print('after timeout'); function __shutdown() print('shutdown'); end; Delay(100,function() end)");
        runtime.shutdown(); runtime.shutdown();
        require(runtime.pending_tasks() == 0 && runtime.callbacks().closed(), "shutdown retained callbacks/tasks");
        require(runtime.take_logs() == std::vector<std::string>{"after timeout", "shutdown"}, "shutdown was not exactly once");
        failed = false;
        try { runtime.run("return"); } catch (const std::runtime_error&) { failed = true; }
        require(failed, "shutdown runtime accepted execution");
        {
            Runtime broken;
            broken.run("function __shutdown() error('shutdown failure') end; Delay(10,function() end)");
            failed = false;
            try { broken.shutdown(); } catch (const std::runtime_error&) { failed = true; }
            require(failed && broken.callbacks().closed() && broken.pending_tasks() == 0, "failed shutdown did not release resources");
            broken.shutdown();
        }
        {
            Runtime callbacks({}, RuntimeOptions{1, 25, 0});
            callbacks.run("events.presentQueue:Add(function() while true do end end)");
            const auto start = std::chrono::steady_clock::now();
            const auto result = callbacks.callbacks().dispatch(Queue::present);
            require(result.lua_status != LUA_OK && result.error.find("25 ms") != std::string::npos,
                    "callback ignored configured deadline");
            require(std::chrono::steady_clock::now() - start < std::chrono::seconds(1), "callback used default deadline");
        }
        {
            Runtime lookup;
            lookup.run("Delay(100,function()end); setmetatable(_G,{__index=function(_,k) if k=='__shutdown' then error('shutdown lookup failed') end end})");
            failed = false;
            try { lookup.shutdown(); }
            catch (const std::runtime_error& error) { failed = std::string(error.what()).find("shutdown lookup failed") != std::string::npos; }
            require(failed && lookup.pending_tasks() == 0 && lookup.callbacks().closed() && lua_gettop(lookup.state()) == 0,
                    "shutdown global lookup escaped protection or retained resources");
        }
        {
            lua_State* state = luaL_newstate();
            luaL_openlibs(state);
            Services services(state);
            require(luaL_dostring(state, "Delay(0,function() print('kept') end)") == LUA_OK, "test setup failed");
            failed = false;
            try { services.dispatch_due(0); } catch (const std::invalid_argument&) { failed = true; }
            require(failed && services.pending() == 1, "invalid deadline consumed a scheduled task");
            require(services.dispatch_due() == 1 && services.take_logs() == std::vector<std::string>{"kept"},
                    "task was lost after rejected dispatch");
            lua_pushlightuserdata(state, &services);
            lua_pushcclosure(state, [](lua_State* current) -> int {
                static_cast<Services*>(lua_touserdata(current, lua_upvalueindex(1)))->close();
                return 0;
            }, 1);
            lua_setglobal(state, "close_services");
            require(luaL_dostring(state, "ran_after_close=false; Delay(0,close_services); Delay(0,function() ran_after_close=true end)") == LUA_OK,
                    "close-during-dispatch setup failed");
            require(services.dispatch_due() == 1 && services.pending() == 0, "close did not stop delayed-task snapshot");
            lua_getglobal(state, "ran_after_close");
            require(!lua_toboolean(state, -1), "delayed task ran after service shutdown");
            lua_pop(state, 1);
            services.close();
            lua_close(state);
        }
        require(protected_call != nullptr, "protected call unavailable");
        std::cout << "Recovered print/unpack, float-clock Delay, nested scheduling, error cleanup, watchdog and shutdown checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
