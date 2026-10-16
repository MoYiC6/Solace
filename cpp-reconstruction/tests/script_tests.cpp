#include <solace/script.hpp>
#include <cmath>
#include <iostream>
#include <stdexcept>

int main() {
    try {
        solace::script::Runtime runtime;
        runtime.run(R"(
            assert(_VERSION == 'Lua 5.4')
            assert(math.Lerp(10, 20, 0.25) == 12.5)
            assert(math.Lerp(0, 10, 2) == 20)
            assert(math.Clamp(5, 10, 0) == 5)
            assert(math.Clamp(-1, 10, 0) == 0)
            assert(math.Clamp(11, 0, 10) == 10)
            assert(math.RemapVal(5, 0, 10, 100, 200) == 150)
            assert(math.RemapVal(9, 5, 5, 100, 200) == 100)
            assert(math.RemapValClamped(20, 0, 10, 100, 200) == 200)
            assert(math.RemapValClamped(-20, 0, 10, 100, 200) == 100)
            assert(utils.Fnv1a('') == 0x4bf29ce484222325)
            assert(utils.Fnv1a('hello') == 0x2430d84680aabd0b)
            assert(utils.Fnv1a('a\0b') ~= utils.Fnv1a('a'))
            assert(io == nil and os.execute == nil and package.loadlib == nil)
            assert(gui == nil and game == nil and ffi == nil)
            assert(not pcall(math.Lerp, {}, 1, 2))
        )");
        bool failed = false;
        try { runtime.run("error('test error')"); } catch (const std::runtime_error&) { failed = true; }
        if (!failed) throw std::runtime_error("Lua errors were not propagated");
        runtime.run("assert(math.Lerp(2, 4, 0.5) == 3)");
        std::cout << "Lua 5.4.7 runtime, 4 math helpers and FNV checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
