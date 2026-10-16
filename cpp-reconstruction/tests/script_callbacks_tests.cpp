#include <solace/script_callbacks.hpp>
#include <lua.hpp>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>

using solace::script::CallbackQueues;
using solace::script::Queue;
namespace {
int direct_call(lua_State* state, int nargs, int nresults) { return lua_pcall(state, nargs, nresults, 0); }
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void run(lua_State* state, const char* source) {
    if (luaL_dostring(state, source) != LUA_OK) {
        const char* text = lua_tostring(state, -1);
        std::string message = text ? text : "non-string test error";
        lua_pop(state, 1);
        throw std::runtime_error(message);
    }
}
struct Fixture {
    lua_State* state{luaL_newstate()};
    std::unique_ptr<CallbackQueues> queues;
    explicit Fixture(std::uint32_t owner = 17) {
        require(state != nullptr, "Lua allocation");
        luaL_openlibs(state);
        queues = std::make_unique<CallbackQueues>(state, owner, direct_call);
    }
    ~Fixture() { queues.reset(); if (state) lua_close(state); }
    void script(const char* source) { run(state, source); }
    solace::script::DispatchResult dispatch(Queue queue = Queue::present, int nargs = 0) {
        return queues->dispatch(queue, nargs);
    }
};
int shutdown_owner(lua_State* state) {
    auto* queues = static_cast<CallbackQueues*>(lua_touserdata(state, lua_upvalueindex(1)));
    queues->close();
    return 0;
}
int deactivate_owner(lua_State* state) {
    auto* queues = static_cast<CallbackQueues*>(lua_touserdata(state, lua_upvalueindex(1)));
    queues->deactivate();
    return 0;
}
void install_host_function(Fixture& fixture, const char* name, lua_CFunction function) {
    lua_pushlightuserdata(fixture.state, fixture.queues.get());
    lua_pushcclosure(fixture.state, function, 1);
    lua_setglobal(fixture.state, name);
}
int scenarios = 0;
template<class Function> void scenario(const char* name, Function function) {
    try { function(); ++scenarios; }
    catch (const std::exception& error) { throw std::runtime_error(std::string(name) + ": " + error.what()); }
}
}

int main() {
    try {
        scenario("six queue contract", [] {
            Fixture f;
            f.script(R"(
                local count = 0
                for _, name in ipairs{'presentQueue','createMove','frameStageNotify','event','input','menuToggled'} do
                    local q = events[name]
                    assert(type(q) == 'userdata' and tostring(q) == 'event queue')
                    assert(getmetatable(q) == 'protected queue')
                    local mt = debug.getmetatable(q)
                    assert(mt.methods == mt.__index and type(q.Add) == 'function' and type(q.Remove) == 'function')
                    count = count + 1
                end
                local actual = 0; for _ in pairs(events) do actual = actual + 1 end
                assert(actual == count and debug.getregistry().__lua_script_id == 17)
            )");
            require(f.queues->owner_id() == 17 && f.queues->active() && !f.queues->closed(), "owner state");
        });
        scenario("six independent queues", [] {
            Fixture f;
            f.script(R"(
                seen = {}
                for name, q in pairs(events) do q:Add(function(...) seen[name] = table.pack(...) end) end
            )");
            for (unsigned i = 0; i < 6; ++i) {
                lua_pushinteger(f.state, i);
                require(f.dispatch(static_cast<Queue>(i), 1).invoked == 1, "queue count");
            }
            f.script("assert(seen.presentQueue[1]==0 and seen.createMove[1]==1 and seen.frameStageNotify[1]==2 and seen.event[1]==3 and seen.input[1]==4 and seen.menuToggled[1]==5)");
        });
        scenario("no returns and type checking", [] {
            Fixture f;
            f.script(R"(
                local q = events.presentQueue; local fn = function() end
                assert(select('#', q:Add(fn, 123)) == 0)
                assert(select('#', q:Remove(fn, 123)) == 0)
                assert(not pcall(q.Add, q, {}) and not pcall(q.Remove, q, nil))
                assert(not pcall(q.Add, {}, fn) and not pcall(q.Remove, false, fn))
            )");
        });
        scenario("duplicates and first removal", [] {
            Fixture f;
            f.script("n=0; fn=function() n=n+1 end; events.presentQueue:Add(fn); events.presentQueue:Add(fn)");
            require(f.dispatch().invoked == 2, "duplicate dispatch");
            f.script("assert(n==2); events.presentQueue:Remove(fn)");
            require(f.queues->size(Queue::present) == 1 && f.dispatch().invoked == 1, "first removal");
            f.script("assert(n==3); events.presentQueue:Remove(fn)");
            require(f.dispatch().invoked == 0, "second removal");
        });
        scenario("identity and missing removal", [] {
            Fixture f;
            f.script("n=0; local function make() return function() n=n+1 end end; fn=make(); events.presentQueue:Add(fn); events.presentQueue:Remove(make())");
            require(f.dispatch().invoked == 1, "closure identity");
            f.script("assert(n==1)");
        });
        scenario("insertion order", [] {
            Fixture f;
            f.script("order=''; for _,v in ipairs{'a','b','c'} do events.presentQueue:Add(function() order=order..v end) end");
            f.dispatch(); f.script("assert(order=='abc')");
        });
        scenario("remove future callback", [] {
            Fixture f;
            f.script("order=''; later=function() order=order..'b' end; events.presentQueue:Add(function() order=order..'a'; events.presentQueue:Remove(later) end); events.presentQueue:Add(later)");
            require(f.dispatch().invoked == 1, "removed snapshot entry skipped");
            f.script("assert(order=='a')");
        });
        scenario("self removal", [] {
            Fixture f;
            f.script("order=''; self=function() order=order..'a'; events.presentQueue:Remove(self) end; events.presentQueue:Add(self); events.presentQueue:Add(function() order=order..'b' end)");
            require(f.dispatch().invoked == 2 && f.dispatch().invoked == 1, "self removal traversal");
            f.script("assert(order=='abb')");
        });
        scenario("new reference deferred", [] {
            Fixture f;
            f.script("order=''; events.presentQueue:Add(function() order=order..'a'; events.presentQueue:Add(function() order=order..'b' end) end)");
            require(f.dispatch().invoked == 1, "new ref absent from snapshot");
            f.script("assert(order=='a')");
            require(f.dispatch().invoked == 2, "new ref next dispatch");
            f.script("assert(order=='aab')");
        });
        scenario("removed reference reused in same dispatch", [] {
            Fixture f;
            f.script("order=''; later=function() order=order..'b' end; events.presentQueue:Add(function() order=order..'a'; events.presentQueue:Remove(later); events.presentQueue:Add(function() order=order..'c' end) end); events.presentQueue:Add(later)");
            require(f.dispatch().invoked == 2, "reused ref is live");
            f.script("assert(order=='ac')");
        });
        scenario("cross queue registration", [] {
            Fixture f;
            f.script("n=0; events.presentQueue:Add(function() events.event:Add(function() n=n+1 end) end)");
            f.dispatch(); f.script("assert(n==0)");
            require(f.dispatch(Queue::event).invoked == 1, "cross queue explicit dispatch");
        });
        scenario("argument identity nil and stack", [] {
            Fixture f;
            f.script(R"(
                events.input:Add(function(t, missing, bytes) assert(missing==nil and bytes=='a\0b'); t.x=42 end)
                events.input:Add(function(t, ...) assert(t.x==42 and select('#',...)==2) end)
            )");
            lua_pushliteral(f.state, "sentinel");
            lua_newtable(f.state); lua_pushnil(f.state); lua_pushlstring(f.state, "a\0b", 3);
            require(f.dispatch(Queue::input, 3).invoked == 2, "argument dispatch");
            require(lua_gettop(f.state) == 1 && std::string(lua_tostring(f.state, 1)) == "sentinel", "stack preservation");
        });
        scenario("return values discarded", [] {
            Fixture f;
            f.script("events.presentQueue:Add(function() return false, 2, {}, 'x' end)");
            require(f.dispatch().lua_status == LUA_OK && lua_gettop(f.state) == 0, "results discarded");
        });
        scenario("Lua error deactivates all owner queues", [] {
            Fixture f;
            f.script("n=0; events.presentQueue:Add(function() error('queue failure',0) end); events.presentQueue:Add(function() n=n+1 end); events.event:Add(function() n=n+1 end)");
            const auto result = f.dispatch();
            require(result.invoked == 1 && result.lua_status == LUA_ERRRUN && result.error == "queue failure", "error reporting");
            require(!f.queues->active() && !f.queues->closed() && f.dispatch(Queue::event).invoked == 0, "owner deactivated");
            f.script("assert(n==0)");
        });
        scenario("other script survives", [] {
            Fixture first(1), second(2);
            first.script("events.presentQueue:Add(function() error('fail') end)");
            second.script("n=0; events.presentQueue:Add(function() n=n+1 end)");
            first.dispatch();
            require(second.dispatch().invoked == 1 && second.queues->active(), "per-owner isolation");
            second.script("assert(n==1 and debug.getregistry().__lua_script_id==2)");
        });
        scenario("embedded NUL error reporting", [] {
            Fixture f;
            f.script(R"(events.presentQueue:Add(function() error('before\0after',0) end))");
            require(f.dispatch().error == "before", "original strlen behavior");
        });
        scenario("non-string error reporting", [] {
            Fixture f;
            f.script("events.presentQueue:Add(function() error({why='failure'}) end)");
            require(f.dispatch().error == "unknown error (non-string error value)", "non-string report");
        });
        scenario("numeric error reporting", [] {
            Fixture f;
            f.script("events.presentQueue:Add(function() error(123) end)");
            require(f.dispatch().error == "123", "lua_tolstring number conversion");
        });
        scenario("inactive Add retains original behavior", [] {
            Fixture f;
            f.queues->deactivate();
            f.script("fn=function() end; assert(select('#',events.presentQueue:Add(fn))==0)");
            require(f.queues->size(Queue::present) == 1 && f.dispatch().invoked == 0, "inactive registration");
            f.script("events.presentQueue:Remove(fn)");
            require(f.queues->size(Queue::present) == 0, "inactive removal");
        });
        scenario("close releases references and is repeatable", [] {
            Fixture f;
            f.script("weak=setmetatable({}, {__mode='v'}); do local fn=function() end; weak[1]=fn; events.presentQueue:Add(fn) end; collectgarbage(); assert(weak[1]~=nil)");
            f.queues->close(); f.queues->close();
            require(f.queues->closed() && f.queues->size(Queue::present) == 0, "closed state");
            f.script("collectgarbage(); assert(weak[1]==nil); local ok,e=pcall(events.presentQueue.Add,events.presentQueue,function()end); assert(not ok and e:find('the script has already been unloaded',1,true)); assert(select('#',events.presentQueue:Remove(function()end))==0)");
        });
        scenario("owner mismatch rejected", [] {
            Fixture f;
            f.script("fn=function()end; events.presentQueue:Add(fn); debug.getregistry().__lua_script_id=99; assert(not pcall(events.presentQueue.Add,events.presentQueue,fn)); events.presentQueue:Remove(fn)");
            require(f.queues->size(Queue::present) == 1, "mismatch remove no-op");
            f.script("debug.getregistry().__lua_script_id=17; events.presentQueue:Remove(fn)");
            require(f.queues->size(Queue::present) == 0, "owner restored");
        });
        scenario("wrong-size userdata rejected", [] {
            Fixture f;
            lua_newuserdatauv(f.state, 1, 0); luaL_setmetatable(f.state, "lua_queue"); lua_setglobal(f.state, "forged");
            f.script("local q=events.presentQueue; assert(not pcall(q.Add,forged,function()end)); debug.getmetatable(q).__gc(forged); debug.getmetatable(q).__gc({})");
        });
        scenario("queue GC is repeatable", [] {
            Fixture f;
            f.script("local q=events.presentQueue; local gc=debug.getmetatable(q).__gc; gc(q); gc(q); assert(not pcall(q.Add,q,function()end)); assert(select('#',q:Remove(function()end))==0); assert(tostring(q)=='event queue')");
            require(f.queues->active(), "collecting queue does not unload owner");
        });
        scenario("owner GC is repeatable", [] {
            Fixture f;
            f.script(R"(
                for _,value in pairs(debug.getregistry()) do
                    if type(value)=='userdata' and getmetatable(value)=='protected callback owner' then
                        local gc=debug.getmetatable(value).__gc; gc(value); gc(value); break
                    end
                end
                assert(not pcall(events.event.Add,events.event,function()end))
            )");
            require(f.queues->closed(), "anchor invalidates C++ owner");
        });
        scenario("Lua closes before C++ handle", [] {
            Fixture f;
            f.script("events.presentQueue:Add(function()end)");
            lua_close(f.state); f.state = nullptr;
            require(f.queues->closed(), "late handle invalidated");
            f.queues->close();
        });
        scenario("C++ handle destroyed before Lua", [] {
            Fixture f;
            f.queues.reset();
            f.script("assert(not pcall(events.presentQueue.Add,events.presentQueue,function()end)); events.presentQueue:Remove(function()end); collectgarbage()");
        });
        scenario("queue collection does not erase callbacks", [] {
            Fixture f;
            f.script("n=0; events.presentQueue:Add(function() n=n+1 end); events=nil; collectgarbage()");
            require(f.dispatch().invoked == 1, "registry callback retained");
            f.script("assert(n==1)");
        });
        scenario("coroutine registration", [] {
            Fixture f;
            f.script("n=0; local co=coroutine.create(function() events.presentQueue:Add(function() n=n+1 end) end); assert(coroutine.resume(co))");
            require(f.dispatch().invoked == 1, "shared registry coroutine reference");
            f.script("assert(n==1)");
        });
        scenario("host validation preserves stack", [] {
            Fixture f;
            lua_pushliteral(f.state, "kept");
            int rejected = 0;
            try { f.dispatch(static_cast<Queue>(99)); } catch (const std::invalid_argument&) { ++rejected; }
            try { f.dispatch(Queue::present, 2); } catch (const std::invalid_argument&) { ++rejected; }
            try { f.dispatch(Queue::present, -1); } catch (const std::invalid_argument&) { ++rejected; }
            f.queues->close();
            try { f.dispatch(); } catch (const std::logic_error&) { ++rejected; }
            require(rejected == 4 && lua_gettop(f.state) == 1, "invalid host calls");
        });
        scenario("no implicit events and empty dispatch consumes arguments", [] {
            Fixture f;
            f.script("n=0; events.event:Add(function() n=n+1 end); assert(n==0)");
            lua_pushinteger(f.state, 7);
            require(f.dispatch(Queue::present, 1).invoked == 0 && lua_gettop(f.state) == 0, "empty queue");
            f.queues->deactivate(); lua_pushinteger(f.state, 8);
            require(f.dispatch(Queue::event, 1).invoked == 0 && lua_gettop(f.state) == 0, "inactive queue");
            f.script("assert(n==0)");
        });
        scenario("duplicate install fails without damaging owner", [] {
            Fixture f;
            bool rejected = false;
            try { CallbackQueues another(f.state, 17, direct_call); } catch (const std::logic_error&) { rejected = true; }
            require(rejected && f.queues->active(), "duplicate installation");
            f.script("events.presentQueue:Add(function()end)");
            require(f.dispatch().invoked == 1, "first owner intact");
        });
        scenario("mismatched initial owner rejected", [] {
            auto* state = luaL_newstate(); require(state != nullptr, "Lua allocation");
            lua_pushinteger(state, 50); lua_setfield(state, LUA_REGISTRYINDEX, "__lua_script_id");
            bool rejected = false;
            try { CallbackQueues queues(state, 17, direct_call); } catch (const std::invalid_argument&) { rejected = true; }
            const bool balanced = lua_gettop(state) == 0;
            lua_close(state); require(rejected && balanced, "initial owner validation");
        });
        scenario("close during callback stops snapshot", [] {
            Fixture f; install_host_function(f, "close_owner", shutdown_owner);
            f.script("n=0; events.presentQueue:Add(function() close_owner() end); events.presentQueue:Add(function() n=n+1 end)");
            require(f.dispatch().invoked == 1 && f.queues->closed(), "close during dispatch");
            f.script("assert(n==0)");
        });
        scenario("deactivate during callback stops snapshot", [] {
            Fixture f; install_host_function(f, "deactivate_owner", deactivate_owner);
            f.script("n=0; events.presentQueue:Add(function() deactivate_owner() end); events.presentQueue:Add(function() n=n+1 end)");
            require(f.dispatch().invoked == 1 && !f.queues->active(), "deactivate during dispatch");
            f.script("assert(n==0)");
        });
        scenario("large mutation preserves traversal", [] {
            Fixture f;
            f.script("n=0; events.presentQueue:Add(function() for i=1,2000 do events.presentQueue:Add(function()n=n+1 end) end end)");
            require(f.dispatch().invoked == 1 && f.queues->size(Queue::present) == 2001, "vector growth");
            require(f.dispatch().invoked == 2001, "snapshot survives reallocation");
            f.script("assert(n==2000)");
        });
        scenario("registered host event evidence", [] {
            const auto names = solace::script::registered_event_names();
            std::set<std::string_view> unique(names.begin(), names.end());
            require(names.size() == 25 && unique.size() == 25 && names.front() == "player_hurt" && names.back() == "player_falldamage", "event names");
            require(solace::script::queue_name(Queue::frame_stage) == "frameStageNotify", "queue name");
        });
        scenario("host protected-call exception restores stack and deactivates", [] {
            lua_State* state = luaL_newstate();
            require(state != nullptr, "Lua allocation");
            luaL_openlibs(state);
            CallbackQueues queues(state, 1, [](lua_State*, int, int) -> int {
                throw std::runtime_error("host call failure");
            });
            require(luaL_dostring(state, "events.input:Add(function() end)") == LUA_OK, "test registration");
            lua_pushliteral(state, "preserve");
            lua_pushinteger(state, 12);
            bool failed = false;
            try { queues.dispatch(Queue::input, 1); }
            catch (const std::runtime_error& error) { failed = std::string(error.what()) == "host call failure"; }
            require(failed && !queues.active() && lua_gettop(state) == 1 &&
                    std::string(lua_tostring(state, 1)) == "preserve", "C++ exception retained callback stack");
            queues.close();
            lua_close(state);
        });
        std::cout << scenarios << " callback queue scenarios passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
