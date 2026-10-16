#include <solace/script_session.hpp>
#include <lua.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace solace::script;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Scratch {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("solace-session-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch() { std::filesystem::create_directory(root); }
    ~Scratch() { std::error_code error; std::filesystem::remove_all(root, error); }
    void write(const std::filesystem::path& file, const std::string& text) {
        std::ofstream stream(root / file, std::ios::binary); stream << text; stream.close();
        if (!stream) throw std::runtime_error("Writing temporary script failed");
    }
};
int main() {
    try {
        Scratch scratch;
        scratch.write("module.lua", "return {value=41}");
        scratch.write("first.lua", R"(
            local module=require('module')
            assert(module.value==41 and global_from_other_script==nil)
            global_from_other_script='first'
            count=0
            events.presentQueue:Add(function() count=count+1; print('present',count) end)
            events.frameStageNotify:Add(function(stage) assert(stage==12); print('stage',stage) end)
            events.input:Add(function(a,b,c) assert(a==1 and b==2 and c==3); print('input') end)
            events.menuToggled:Add(function(visible) assert(visible==true); print('menu') end)
            Delay(1,function() print('delayed') end)
            function __shutdown() print('closed') end
            print('loaded')
        )");
        scratch.write("second.lua", "assert(global_from_other_script==nil); events.presentQueue:Add(function() print('second') end)");
        Session session(scratch.root);
        const auto first = session.load("first.lua");
        require(session.load("./first.lua") == first && session.scripts().size() == 1, "duplicate load executed twice");
        const auto second = session.load("second.lua");
        require(second != first && session.scripts().size() == 2, "script states not separated");
        auto logs = session.take_logs();
        require(logs.size() == 1 && logs[0].text == "loaded", "load logs lost");
        auto tick = session.tick(0);
        require(tick.callbacks == 2 && tick.delayed_tasks == 0 && tick.failures.empty(), "initial session tick differs");
        session.take_logs();
        tick = session.tick(1);
        logs = session.take_logs();
        require(tick.delayed_tasks == 1 && logs.size() == 3 && logs[0].text == "delayed" && logs[1].text == "present 2", "delay/present ordering differs");
        const std::vector<EventArgument> stage{std::int64_t{12}}, input{std::int64_t{1},std::int64_t{2},std::int64_t{3}}, menu{true};
        require(session.dispatch(Queue::frame_stage,stage).callbacks == 1, "frame stage contract failed");
        require(session.dispatch(Queue::input,input).callbacks == 1, "input contract failed");
        require(session.dispatch(Queue::menu_toggled,menu).callbacks == 1, "menu contract failed");
        bool failed = false;
        try { session.dispatch(Queue::event); } catch (const std::invalid_argument&) { failed = true; }
        require(failed, "missing game event host was faked");
        failed = false;
        try { session.dispatch(Queue::menu_toggled,stage); } catch (const std::invalid_argument&) { failed = true; }
        require(failed, "wrong event argument accepted");
        session.take_logs();
        const auto reloaded = session.reload(first);
        logs = session.take_logs();
        require(reloaded != first && logs.size() == 2 && logs[0].text == "closed" && logs[1].text == "loaded", "reload lifecycle differs");
        scratch.write("bad.lua", "events.presentQueue:Add(function() error('bad callback') end); function __shutdown() error('should not run after fault') end");
        const auto bad = session.load("bad.lua");
        tick = session.tick(1);
        require(tick.failures.size() == 1 && tick.failures[0].id == bad && session.scripts().size() == 2, "failed script was not isolated and removed");
        scratch.write("syntax.lua", "this is not lua (");
        failed = false;
        try { session.load("syntax.lua"); } catch (const std::runtime_error&) { failed = true; }
        require(failed && session.scripts().size() == 2, "failed load retained live state");
        scratch.write("bad_shutdown.lua", "function __shutdown() error('shutdown failed') end");
        session.load("bad_shutdown.lua");
        const auto closed = session.close();
        require(closed.failures.size() == 1 && session.scripts().empty(), "close did not finish other scripts after shutdown error");
        require(session.close().failures.empty(), "close is not idempotent");
        // File loading accepts official matching-version Lua bytecode, as original.
        Runtime compiler;
        require(luaL_loadstring(compiler.state(), "print('bytecode')") == LUA_OK, "test compilation failed");
        std::string bytecode;
        const auto writer = [](lua_State*, const void* p, std::size_t n, void* out) -> int {
            static_cast<std::string*>(out)->append(static_cast<const char*>(p), n); return 0;
        };
        require(lua_dump(compiler.state(),writer,&bytecode,0)==0,"test bytecode dump failed");
        scratch.write("compiled.luac",bytecode);
        session.load("compiled.luac");
        logs=session.take_logs();
        require(!logs.empty() && logs.back().text=="bytecode","original bytecode load mode not restored");
        session.close();
        scratch.write("deleted.lua", "print('before deletion')");
        scratch.write("after_deletion.lua", "print('after deletion')");
        session.load("deleted.lua");
        std::filesystem::remove(scratch.root / "deleted.lua");
        session.load("after_deletion.lua");
        require(session.scripts().size() == 2, "deleted source blocked another script load");
        session.close();
        std::cout << "Independent script states, module import, duplicate load, callbacks, errors, reload, shutdown and bytecode checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
