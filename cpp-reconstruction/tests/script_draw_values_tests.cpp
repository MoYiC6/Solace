#include <solace/script_draw_values.hpp>
#include <solace/script_math.hpp>
#include <lua.hpp>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void run(lua_State* L, const char* source) {
    if (luaL_loadstring(L, source) != LUA_OK || lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK)
        throw std::runtime_error(lua_tostring(L, -1));
}
std::string bytes_hex(const void* data, std::size_t size) {
    constexpr char digits[] = "0123456789abcdef";
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::string result;
    for (std::size_t i = 0; i < size; ++i) {
        result += digits[bytes[i] >> 4]; result += digits[bytes[i] & 15];
    }
    return result;
}
}

int main() {
    lua_State* L = luaL_newstate();
    if (!L) { std::cerr << "Lua allocation failed\n"; return 1; }
    try {
        luaL_openlibs(L);
        solace::script::register_math_extensions(L);
        solace::script::register_draw_values(L);
        if (lua_gettop(L) != 0) throw std::runtime_error("Registration leaked Lua stack values");
        solace::script::push_bits(L, 0);
        lua_setglobal(L, "test_bits");
        run(L, R"lua(
checks = 0
local function check(v) checks = checks + 1; assert(v, 'draw check '..checks) end
local function fails(f, text)
    local ok, err = pcall(f)
    check(not ok and (not text or tostring(err):find(text, 1, true)))
end
check(type(draw) == 'table' and draw.Vec2 == math.Vec2 and draw.vec2 == draw.Vec2)
check(draw.Rect == draw.rect and draw.Color == draw.color.RGBA)
check(draw.color.BGRA == draw.color.RGBA and draw.color.ABGR == draw.color.ARGB)
local c = draw.Color(10, 20, 30, 40)
check(getmetatable(c) == 'protected color')
check(tostring(c) == 'color(10, 20, 30, 40)')
check(c:R() == 10 and c:GetG() == 20 and c:B(nil) == 30 and c:A() == 40)
check(c:R(12.5):R() == 13 and c:R() == 10)
)lua");
        lua_settop(L, 0);
        run(L, R"lua(
local function check(v) checks = checks + 1; assert(v, 'draw check '..checks) end
local function fails(f, text)
    local ok, err = pcall(f)
    check(not ok and (not text or tostring(err):find(text, 1, true)))
end
local c = draw.Color(10, 20, 30, 40)
check(c.R == c.GetR and c.G == c.GetG and c.B == c.GetB and c.A == c.GetA)
check(c.Interpolate == draw.color.Interpolate)
check(c == draw.Color(10,20,30,40) and c ~= draw.Color(10,20,30,41))
check(draw.Color() == draw.color.White())
check(draw.Color(false,{},'bad',nil) == draw.color.White())
check(draw.Color('12.5',-100,300,.49) == draw.Color(13,0,255,0))
check(draw.color.ARGB(40,10,20,30) == c)
check(draw.color.WhiteTransparent() == draw.Color(255,255,255,0))
check(draw.color.Black() == draw.Color(0,0,0,255))
check(draw.color.BlackTransparent() == draw.Color(0,0,0,0))
check(draw.color.Gray() == draw.Color(128,128,128,255))
check(draw.color.Percent(.5,12.5) == draw.Color(127,127,127,13))
check(c:Darken(.5) == draw.Color(5,10,15,40))
check(c:Lighten(.5) == draw.Color(133,138,143,40))
check(c:ModA(.333) == draw.Color(10,20,30,13))
check(draw.color.Interpolate(c, draw.Color(11,23,35,47), .5) == draw.Color(10,21,32,43))
check(draw.color.HSV() == draw.color.Black())
check(draw.color.HSV(0,1,1) == draw.Color(255,0,0,255))
check(draw.color.HSV(-60,1,1,.5) == draw.Color(255,0,255,127))
check(draw.color.HSV(120,1,1) == draw.Color(0,255,0,255))
fails(function() c.R = 1 end)
fails(function() c:R(false) end, 'number expected')
fails(function() c:Darken() end, 'number expected')
fails(function() draw.color.HSV('bad') end, 'number expected')
fails(function() draw.color.Interpolate(c, {}, .5) end, 'lua_color expected')
local r = draw.Rect('1.5',2,10,20)
check(getmetatable(r) == 'protected rect' and r.x == 1.5 and r.y == 2 and r.w == 10 and r.h == 20)
local clone = r:Clone()
check(clone ~= r and clone.x == r.x and clone.h == r.h)
check(r:Contains(draw.Vec2(1.5,2)) and r:Contains(draw.Vec2(11.5,22)))
check(not r:Contains(draw.Vec2(11.5001,22)))
check(not draw.Rect(0,0,-2,10):Contains(draw.Vec2(0,0)))
local offset = r:Offset(draw.Vec2(2,-3))
check(offset.x == 3.5 and offset.y == -1 and offset.w == 10 and r.x == 1.5)
local expanded = r:Expand(2)
check(expanded.x == -.5 and expanded.y == 0 and expanded.w == 14 and expanded.h == 24)
local shrunk = r:Shrink(100)
check(shrunk.x == 101.5 and shrunk.y == 102 and shrunk.w == 0 and shrunk.h == 0)
r.x = '4.5'; r.y = false; r.w = nil; r.h = 'invalid'
check(r.x == 4.5 and r.y == 0 and r.w == 0 and r.h == 0)
r.unrecognized = 99; r[{}] = 99
check(r.unrecognized == nil and r[{}] == nil and r[123] == nil)
check(r['x\0ignored'] == r.x)
check(draw.Rect(false,{},'bad',nil).w == 0)
fails(function() r:Contains({x=1,y=2}) end, 'lua_vec2')
fails(function() r:Offset(draw.Rect()) end, 'lua_vec2')
fails(function() r:Expand(false) end, 'number expected')
check(getmetatable(test_bits) == 'protected bits' and tostring(test_bits) == 'bits(0)')
check(test_bits:GetRaw() == 0 and not test_bits:Get(1) and not test_bits:Get(64))
check(select('#', test_bits:Set(64,true)) == 0)
check(test_bits:Get(64) and test_bits:GetRaw() == math.mininteger)
check(tostring(test_bits) == 'bits(9223372036854775808)')
test_bits:Set(1,0); check(test_bits:Get(1)) -- Lua zero is truthy.
test_bits:Set(1); check(not test_bits:Get(1))
test_bits:Toggle(64); check(test_bits:GetRaw() == 0)
test_bits:SetRaw(-1); check(test_bits:Get(1) and test_bits:Get(64))
test_bits:Unset(1); check(not test_bits:Get(1))
local raw = test_bits:GetRaw(); test_bits:Unset(0); test_bits:Toggle(65)
check(test_bits:GetRaw() == raw)
fails(function() test_bits:Get(0) end, 'bits index out of range (1..64)')
fails(function() test_bits:Set(65,true) end, 'bits index out of range (1..64)')
fails(function() test_bits:Get(1.5) end, 'integer representation')
fails(function() test_bits:SetRaw({}) end, 'number expected')
local empty = test_bits:None(); check(empty:GetRaw() == 0 and test_bits:GetRaw() ~= 0)
test_bits:Reset(); check(test_bits:GetRaw() == 0)
local bad_rect = draw.Color()
debug.setmetatable(bad_rect, debug.getmetatable(draw.Rect()))
fails(function() bad_rect:Clone() end, 'invalid userdata size')
local bad_color = draw.Rect()
debug.setmetatable(bad_color, debug.getmetatable(draw.Color()))
fails(function() bad_color:R() end, 'invalid userdata size')
local bad_bits = draw.Color()
debug.setmetatable(bad_bits, debug.getmetatable(test_bits))
fails(function() bad_bits:Set(1,true) end, 'invalid userdata size')
for _, name in ipairs({'surface','fonts','textures','Texture','CreateTexture','GetTime','GetFrameTime','GetDisplay','GetScale'}) do
    check(draw[name] == nil)
end
)lua");
        lua_settop(L, 0);
        const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
                          "tests/fixtures/draw-machine-vectors.json";
        std::ifstream input(path);
        if (!input) throw std::runtime_error("Missing original-machine-code fixture: " + path.string());
        const auto report = nlohmann::json::parse(input);
        if (report.at("source_sha256") != "8e60e1071acd82b08dfd2bbc41b1155e10cfd113075f0ff40ed6372c2f5f232b")
            throw std::runtime_error("Wrong machine fixture source identity");
        std::size_t machine_checks = 0;
        for (const auto& item : report.at("cases")) {
            const auto source = item.at("lua").get<std::string>();
            run(L, source.c_str());
            const auto& expected = item.at("expected");
            const auto kind = expected.at("kind").get<std::string>();
            bool match = false;
            if (kind == "Color" || kind == "Rect") {
                const auto* pointer = luaL_testudata(L, -1, kind == "Color" ? "lua_color" : "lua_rect");
                match = pointer && bytes_hex(pointer, kind == "Color" ? 4 : 16) == expected.at("hex").get<std::string>();
            } else if (kind == "boolean") {
                match = lua_isboolean(L, -1) && (lua_toboolean(L, -1) != 0) == expected.at("value").get<bool>();
            } else if (kind == "integer") {
                match = lua_isinteger(L, -1) && lua_tointeger(L, -1) == expected.at("value").get<lua_Integer>();
            }
            if (!match) throw std::runtime_error("Original machine result mismatch: " + source + " expected=" + expected.dump() +
                " actual=" + (lua_isuserdata(L, -1) ? bytes_hex(lua_touserdata(L, -1), lua_rawlen(L, -1)) : "non-userdata"));
            ++machine_checks;
            lua_settop(L, 0);
        }
        if (machine_checks != report.at("count").get<std::size_t>()) throw std::runtime_error("Fixture count mismatch");
        solace::script::register_draw_values(L);
        if (lua_gettop(L) != 0) throw std::runtime_error("Repeated registration leaked Lua stack values");
        lua_getglobal(L, "checks");
        std::cout << "Lua draw values: " << lua_tointeger(L, -1) << " behavior checks and "
                  << machine_checks << " original-machine-code checks passed\n";
        lua_close(L);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        lua_close(L);
        return 1;
    }
}
