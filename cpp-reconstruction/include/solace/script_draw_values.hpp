#pragma once
#include <cstdint>

struct lua_State;

namespace solace::script {
struct Rect { float x{}, y{}, w{}, h{}; };
struct Color { std::uint8_t r{}, g{}, b{}, a{}; };
static_assert(sizeof(Rect) == 16 && sizeof(Color) == 4);

// Full userdata with the recovered lua_rect/lua_color/lua_bits metatables.
// check_* raises a Lua argument error; call before owning RAII locals.
Rect check_rect(lua_State*, int index);
Color check_color(lua_State*, int index);
std::uint64_t check_bits(lua_State*, int index);
void push_rect(lua_State*, Rect value);
void push_color(lua_State*, Color value);
void push_bits(lua_State*, std::uint64_t value);

// Call register_math_extensions first. Installs only evidenced pure draw values;
// no layer, texture, font, timing, display, or rendering host is manufactured.
void register_draw_values(lua_State*);
}
