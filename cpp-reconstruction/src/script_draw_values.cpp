#include <solace/script_draw_values.hpp>
#include <solace/script_math.hpp>
#include <lua.hpp>
#include <bit>
#include <cmath>
#include <cstdio>
#include <new>

namespace solace::script {
namespace {
constexpr const char* rect_meta = "lua_rect";
constexpr const char* color_meta = "lua_color";
constexpr const char* bits_meta = "lua_bits";
constexpr double above_one = 1.000000059604644775390625;
constexpr double below_zero = -0x1p-150;

template<class T> T* test_value(lua_State* L, int index, const char* metatable) {
    auto* value = static_cast<T*>(luaL_testudata(L, index, metatable));
    return value && lua_rawlen(L, index) == sizeof(T) ? value : nullptr;
}
template<class T> T* checked_value(lua_State* L, int index, const char* metatable) {
    auto* value = static_cast<T*>(luaL_checkudata(L, index, metatable));
    if (lua_rawlen(L, index) != sizeof(T)) luaL_argerror(L, index, "invalid userdata size");
    return value;
}

float optional_coordinate(lua_State* L, int index) {
    return lua_isnumber(L, index) ? static_cast<float>(lua_tonumber(L, index)) : 0.0f;
}
// The original CVTT conversion produces INT_MIN for NaN; its low byte is zero.
// Explicit comparisons avoid an undefined C++ floating-to-integer conversion.
std::uint8_t trunc_byte(float value) {
    if (!(value > 0.0f)) return 0;
    if (value >= 255.0f) return 255;
    return static_cast<std::uint8_t>(value);
}
std::uint8_t rounded_byte(double value) {
    value = std::round(value);
    if (!(value > 0.0)) return 0;
    if (value >= 255.0) return 255;
    return static_cast<std::uint8_t>(value);
}
std::uint8_t optional_channel(lua_State* L, int index) {
    return lua_isnumber(L, index) ? rounded_byte(lua_tonumber(L, index)) : 255;
}
float unit_float(float value) {
    if (value > 1.0f) return 1.0f;
    return value < 0.0f ? 0.0f : value;
}
int rect_constructor(lua_State* L) {
    const Rect value{optional_coordinate(L, 1), optional_coordinate(L, 2),
                     optional_coordinate(L, 3), optional_coordinate(L, 4)};
    push_rect(L, value);
    return 1;
}
int rect_clone(lua_State* L) { push_rect(L, check_rect(L, 1)); return 1; }
int rect_contains(lua_State* L) {
    const Rect value = check_rect(L, 1);
    const Vec2 point = check_vec2(L, 2);
    lua_pushboolean(L, point.x >= value.x && value.x + value.w >= point.x &&
                       point.y >= value.y && value.y + value.h >= point.y);
    return 1;
}
int rect_offset(lua_State* L) {
    const Rect value = check_rect(L, 1);
    const Vec2 offset = check_vec2(L, 2);
    push_rect(L, {offset.x + value.x, offset.y + value.y, value.w, value.h});
    return 1;
}
int rect_expand(lua_State* L) {
    const Rect value = check_rect(L, 1);
    const float amount = static_cast<float>(luaL_checknumber(L, 2));
    const float twice = amount + amount;
    push_rect(L, {value.x - amount, value.y - amount, twice + value.w, twice + value.h});
    return 1;
}
int rect_shrink(lua_State* L) {
    const Rect value = check_rect(L, 1);
    const float amount = static_cast<float>(luaL_checknumber(L, 2));
    const float twice = amount + amount;
    const float width = value.w - twice;
    const float height = value.h - twice;
    // MAXPS(result, 0) chooses zero for NaN and equal operands as well.
    push_rect(L, {value.x + amount, value.y + amount,
                  width > 0.0f ? width : 0.0f, height > 0.0f ? height : 0.0f});
    return 1;
}
float* rect_field(Rect* value, const char* key) {
    if (value == nullptr || key == nullptr || key[0] == '\0' || key[1] != '\0') return nullptr;
    switch (key[0]) {
    case 'x': return &value->x;
    case 'y': return &value->y;
    case 'w': return &value->w;
    case 'h': return &value->h;
    default: return nullptr;
    }
}
int rect_index(lua_State* L) {
    const char* key = lua_tostring(L, 2);
    if (key == nullptr) return 0;
    auto* value = test_value<Rect>(L, 1, rect_meta);
    if (const float* field = rect_field(value, key)) {
        lua_pushnumber(L, *field);
    } else {
        luaL_getmetatable(L, rect_meta);
        lua_getfield(L, -1, "methods");
        lua_pushvalue(L, 2);
        lua_rawget(L, -2);
    }
    return 1;
}
int rect_newindex(lua_State* L) {
    const char* key = lua_tostring(L, 2);
    auto* value = test_value<Rect>(L, 1, rect_meta);
    if (float* field = rect_field(value, key)) *field = static_cast<float>(lua_tonumber(L, 3));
    return 0;
}
int color_constructor(lua_State* L) {
    const Color value{optional_channel(L, 1), optional_channel(L, 2),
                      optional_channel(L, 3), optional_channel(L, 4)};
    push_color(L, value);
    return 1;
}
int color_argb(lua_State* L) {
    const Color value{optional_channel(L, 2), optional_channel(L, 3),
                      optional_channel(L, 4), optional_channel(L, 1)};
    push_color(L, value);
    return 1;
}
template<unsigned Channel> int color_channel(lua_State* L) {
    Color value = check_color(L, 1);
    auto* bytes = reinterpret_cast<std::uint8_t*>(&value);
    if (lua_isnoneornil(L, 2)) lua_pushinteger(L, bytes[Channel]);
    else {
        bytes[Channel] = rounded_byte(luaL_checknumber(L, 2));
        push_color(L, value);
    }
    return 1;
}
int color_darken(lua_State* L) {
    Color value = check_color(L, 1);
    const double amount = luaL_checknumber(L, 2);
    const float factor = amount > above_one ? 0.0f :
                         amount < below_zero ? 1.0f : 1.0f - static_cast<float>(amount);
    value.r = trunc_byte(std::round(static_cast<float>(value.r) * factor));
    value.g = trunc_byte(std::round(static_cast<float>(value.g) * factor));
    value.b = trunc_byte(std::round(static_cast<float>(value.b) * factor));
    push_color(L, value);
    return 1;
}
int color_lighten(lua_State* L) {
    Color value = check_color(L, 1);
    const double amount = luaL_checknumber(L, 2);
    const float factor = amount > above_one ? 1.0f :
                         amount < below_zero ? 0.0f : static_cast<float>(amount);
    const auto transform = [factor](std::uint8_t channel) {
        const float first = static_cast<float>(channel);
        const float scaled = (255.0f - first) * factor;
        return trunc_byte(std::round(scaled + first));
    };
    value.r = transform(value.r); value.g = transform(value.g); value.b = transform(value.b);
    push_color(L, value);
    return 1;
}
int color_mod_a(lua_State* L) {
    Color value = check_color(L, 1);
    const float factor = static_cast<float>(luaL_checknumber(L, 2));
    value.a = trunc_byte(static_cast<float>(value.a) * factor);
    push_color(L, value);
    return 1;
}
int color_interpolate(lua_State* L) {
    const Color first = check_color(L, 1);
    const Color last = check_color(L, 2);
    const double input = luaL_checknumber(L, 3);
    const float amount = input > above_one ? 1.0f : unit_float(static_cast<float>(input));
    const auto channel = [amount](std::uint8_t a, std::uint8_t b) {
        const float scaled = amount * static_cast<float>(static_cast<int>(b) - a);
        return trunc_byte(scaled + static_cast<float>(a));
    };
    push_color(L, {channel(first.r, last.r), channel(first.g, last.g),
                  channel(first.b, last.b), channel(first.a, last.a)});
    return 1;
}
int color_equal(lua_State* L) {
    const auto* a = test_value<Color>(L, 1, color_meta);
    const auto* b = test_value<Color>(L, 2, color_meta);
    lua_pushboolean(L, a && b && a->r == b->r && a->g == b->g && a->b == b->b && a->a == b->a);
    return 1;
}
int color_string(lua_State* L) {
    const Color c = check_color(L, 1);
    lua_pushfstring(L, "color(%d, %d, %d, %d)", static_cast<int>(c.r), static_cast<int>(c.g),
                    static_cast<int>(c.b), static_cast<int>(c.a));
    return 1;
}
template<std::uint8_t Gray, std::uint8_t Alpha> int color_gray(lua_State* L) {
    push_color(L, {Gray, Gray, Gray, Alpha}); return 1;
}
int color_percent(lua_State* L) {
    const double input = luaL_checknumber(L, 1);
    const float percent = static_cast<float>(input);
    const std::uint8_t alpha = optional_channel(L, 2);
    const std::uint8_t gray = input > above_one ? 255 : trunc_byte(percent * 255.0f);
    push_color(L, {gray, gray, gray, alpha});
    return 1;
}
int color_hsv(lua_State* L) {
    float hue = static_cast<float>(luaL_optnumber(L, 1, 0));
    const float saturation = unit_float(static_cast<float>(luaL_optnumber(L, 2, 0)));
    const float value = unit_float(static_cast<float>(luaL_optnumber(L, 3, 0)));
    const float alpha = unit_float(static_cast<float>(luaL_optnumber(L, 4, 1)));
    hue = std::fmod(hue, 360.0f);
    if (hue < 0.0f) hue += 360.0f;
    const float chroma = saturation * value;
    const float sector = hue / 60.0f;
    const float x = (1.0f - std::fabs(std::fmod(sector, 2.0f) - 1.0f)) * chroma;
    float red = 0.0f, green = 0.0f, blue = 0.0f;
    if (sector < 1.0f) { red = chroma; green = x; }
    else if (sector < 2.0f) { red = x; green = chroma; }
    else if (sector < 3.0f) { green = chroma; blue = x; }
    else if (sector < 4.0f) { green = x; blue = chroma; }
    else if (sector < 5.0f) { red = x; blue = chroma; }
    else { red = chroma; blue = x; }
    const float minimum = value - chroma;
    push_color(L, {trunc_byte((minimum + red) * 255.0f), trunc_byte((green + minimum) * 255.0f),
                  trunc_byte((blue + minimum) * 255.0f), trunc_byte(alpha * 255.0f)});
    return 1;
}
std::uint64_t* bits_pointer(lua_State* L) {
    return checked_value<std::uint64_t>(L, 1, bits_meta);
}
int bits_none(lua_State* L) { push_bits(L, 0); return 1; }
int bits_get(lua_State* L) {
    const std::uint64_t value = check_bits(L, 1);
    const lua_Integer index = luaL_checkinteger(L, 2);
    if (index < 1 || index > 64) return luaL_error(L, "bits index out of range (1..64)");
    lua_pushboolean(L, (value & (std::uint64_t{1} << (index - 1))) != 0);
    return 1;
}
int bits_set(lua_State* L) {
    auto* value = bits_pointer(L);
    const lua_Integer index = luaL_checkinteger(L, 2);
    const bool enabled = lua_toboolean(L, 3) != 0;
    if (index < 1 || index > 64) return luaL_error(L, "bits index out of range (1..64)");
    const std::uint64_t mask = std::uint64_t{1} << (index - 1);
    *value = enabled ? *value | mask : *value & ~mask;
    return 0;
}
template<bool Toggle> int bits_change(lua_State* L) {
    auto* value = bits_pointer(L);
    const lua_Integer index = luaL_checkinteger(L, 2);
    if (index >= 1 && index <= 64) {
        const std::uint64_t mask = std::uint64_t{1} << (index - 1);
        if constexpr (Toggle) *value ^= mask;
        else *value &= ~mask;
    }
    return 0;
}
int bits_get_raw(lua_State* L) {
    lua_pushinteger(L, std::bit_cast<lua_Integer>(check_bits(L, 1))); return 1;
}
int bits_set_raw(lua_State* L) {
    auto* value = bits_pointer(L); *value = static_cast<std::uint64_t>(luaL_checkinteger(L, 2)); return 0;
}
int bits_reset(lua_State* L) { *bits_pointer(L) = 0; return 0; }
int bits_string(lua_State* L) {
    const std::uint64_t value = check_bits(L, 1);
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "bits(%llu)", static_cast<unsigned long long>(value));
    lua_pushstring(L, buffer);
    return 1;
}
constexpr luaL_Reg rect_methods[] = {{"Clone", rect_clone}, {"Contains", rect_contains},
    {"Offset", rect_offset}, {"Expand", rect_expand}, {"Shrink", rect_shrink}, {nullptr, nullptr}};
constexpr luaL_Reg color_methods[] = {{"R", color_channel<0>}, {"G", color_channel<1>},
    {"B", color_channel<2>}, {"A", color_channel<3>}, {"GetR", color_channel<0>},
    {"GetG", color_channel<1>}, {"GetB", color_channel<2>}, {"GetA", color_channel<3>},
    {"Darken", color_darken}, {"Lighten", color_lighten}, {"ModA", color_mod_a},
    {"Interpolate", color_interpolate}, {nullptr, nullptr}};
constexpr luaL_Reg bits_methods[] = {{"None", bits_none}, {"Get", bits_get}, {"Set", bits_set},
    {"Unset", bits_change<false>}, {"Toggle", bits_change<true>}, {"GetRaw", bits_get_raw},
    {"SetRaw", bits_set_raw}, {"Reset", bits_reset}, {nullptr, nullptr}};
constexpr luaL_Reg color_factories[] = {{"White", color_gray<255, 255>},
    {"WhiteTransparent", color_gray<255, 0>}, {"Black", color_gray<0, 255>},
    {"BlackTransparent", color_gray<0, 0>}, {"Gray", color_gray<128, 255>},
    {"Percent", color_percent}, {"RGBA", color_constructor}, {"ARGB", color_argb},
    {"BGRA", color_constructor}, {"ABGR", color_argb}, {"HSV", color_hsv},
    {"Interpolate", color_interpolate}, {nullptr, nullptr}};
void set_function(lua_State* L, const char* key, lua_CFunction function) {
    lua_pushcfunction(L, function); lua_setfield(L, -2, key);
}
void set_protection(lua_State* L, const char* label) {
    lua_pushstring(L, label); lua_setfield(L, -2, "__metatable");
}
void set_methods(lua_State* L, const luaL_Reg* methods, bool index_table) {
    lua_newtable(L); luaL_setfuncs(L, methods, 0);
    if (index_table) { lua_pushvalue(L, -1); lua_setfield(L, -3, "__index"); }
    lua_setfield(L, -2, "methods");
}
}

Rect check_rect(lua_State* L, int index) {
    return *checked_value<Rect>(L, index, rect_meta);
}
Color check_color(lua_State* L, int index) {
    return *checked_value<Color>(L, index, color_meta);
}
std::uint64_t check_bits(lua_State* L, int index) {
    return *checked_value<std::uint64_t>(L, index, bits_meta);
}
void push_rect(lua_State* L, Rect value) {
    new(lua_newuserdatauv(L, sizeof(Rect), 0)) Rect(value);
    luaL_setmetatable(L, rect_meta);
}
void push_color(lua_State* L, Color value) {
    new(lua_newuserdatauv(L, sizeof(Color), 0)) Color(value);
    luaL_setmetatable(L, color_meta);
}
void push_bits(lua_State* L, std::uint64_t value) {
    new(lua_newuserdatauv(L, sizeof(value), 0)) std::uint64_t(value);
    luaL_setmetatable(L, bits_meta);
}
void register_draw_values(lua_State* L) {
    if (luaL_newmetatable(L, rect_meta)) {
        set_methods(L, rect_methods, false);
        set_function(L, "__index", rect_index); set_function(L, "__newindex", rect_newindex);
        set_protection(L, "protected rect");
    }
    lua_pop(L, 1);
    if (luaL_newmetatable(L, color_meta)) {
        set_methods(L, color_methods, true);
        set_function(L, "__eq", color_equal); set_function(L, "__tostring", color_string);
        set_protection(L, "protected color");
    }
    lua_pop(L, 1);
    if (luaL_newmetatable(L, bits_meta)) {
        set_methods(L, bits_methods, true); set_function(L, "__tostring", bits_string);
        set_protection(L, "protected bits");
    }
    lua_pop(L, 1);
    lua_getglobal(L, "draw");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); lua_newtable(L); }
    set_function(L, "Rect", rect_constructor); set_function(L, "rect", rect_constructor);
    set_function(L, "Color", color_constructor);
    // Vec2 constructor aliases are supplied by the shared math implementation.
    lua_getglobal(L, "math"); lua_getfield(L, -1, "Vec2");
    if (!lua_isfunction(L, -1)) luaL_error(L, "register_math_extensions must precede register_draw_values");
    lua_pushvalue(L, -1); lua_setfield(L, -4, "Vec2"); lua_setfield(L, -3, "vec2");
    lua_pop(L, 1);
    lua_newtable(L); luaL_setfuncs(L, color_factories, 0); lua_setfield(L, -2, "color");
    lua_setglobal(L, "draw");
}
}
