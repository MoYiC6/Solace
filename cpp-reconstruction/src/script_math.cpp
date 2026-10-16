#include <solace/script_math.hpp>
#include <lua.hpp>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <new>
#include <type_traits>

namespace solace::script {
namespace {
constexpr float radians_to_degrees = std::bit_cast<float>(0x42652ee1U);
constexpr float degrees_to_radians = std::bit_cast<float>(0x3c8efa35U);
constexpr float pi = std::bit_cast<float>(0x40490fdbU);
constexpr float minimum_speed = std::bit_cast<float>(0x3a83126fU);
float minss(float a, float b) noexcept { return a < b ? a : b; }
float maxss(float a, float b) noexcept { return a > b ? a : b; }
float wrap_once(float v) noexcept {
    if (v > 180.0f) v += -360.0f;
    if (v < -180.0f) v += 360.0f;
    return v;
}
float optional_number(lua_State* state, int index, float value = 0.0f) {
    return lua_isnumber(state, index) ? static_cast<float>(lua_tonumber(state, index)) : value;
}
template<class V> constexpr const char* metatable() {
    if constexpr (std::is_same_v<V, Vec2>) return "lua_vec2";
    else return "lua_vector";
}
template<class V> constexpr int dimensions = std::is_same_v<V, Vec2> ? 2 : 3;
template<class V> float& component(V& value, int index) {
    if (index == 0) return value.x;
    if constexpr (dimensions<V> == 3) { if (index == 2) return value.z; }
    return value.y;
}
template<class V> V* test(lua_State* state, int index) {
    auto* value = static_cast<V*>(luaL_testudata(state, index, metatable<V>()));
    return value && lua_rawlen(state, index) == sizeof(V) ? value : nullptr;
}
template<class V> V* checked(lua_State* state, int index) {
    auto* value = test<V>(state, index);
    if (!value) luaL_argerror(state, index, metatable<V>());
    return value;
}
template<class V> void push(lua_State* state, V value) {
    new(lua_newuserdatauv(state, sizeof(V), 0)) V(value);
    luaL_setmetatable(state, metatable<V>());
}
int scalar(lua_State* state, float value) { lua_pushnumber(state, value); return 1; }
template<class V> int result(lua_State* state, V value) { push(state, value); return 1; }
template<class V> float square_length(V value) {
    const float xy = value.x * value.x + value.y * value.y;
    if constexpr (dimensions<V> == 3) return value.z * value.z + xy;
    else return xy;
}
template<class V> int clone(lua_State* state) { return result(state, *checked<V>(state, 1)); }
template<class V, bool Squared, bool Only2d = false> int length(lua_State* state) {
    const auto value = *checked<V>(state, 1);
    float sum{};
    if constexpr (Only2d) sum = square_length(Vec2{value.x, value.y});
    else sum = square_length(value);
    return scalar(state, Squared ? sum : std::sqrt(sum));
}
template<class V, bool Squared, bool Only2d = false> int distance(lua_State* state) {
    auto a = *checked<V>(state, 1);
    auto b = *checked<V>(state, 2);
    for (int i = 0; i < dimensions<V>; ++i) component(a, i) -= component(b, i);
    float sum{};
    if constexpr (Only2d) sum = square_length(Vec2{a.x, a.y});
    else sum = square_length(a);
    return scalar(state, Squared ? sum : std::sqrt(sum));
}
template<class V> int dot(lua_State* state) {
    const auto a = *checked<V>(state, 1), b = *checked<V>(state, 2);
    const float xy = a.x * b.x + a.y * b.y;
    if constexpr (dimensions<V> == 3) return scalar(state, a.z * b.z + xy);
    else return scalar(state, xy);
}
template<class V> int index(lua_State* state) {
    const char* key = lua_tostring(state, 2);
    if (!key) return 0;
    const auto* value = test<V>(state, 1);
    const int coordinate = static_cast<unsigned char>(key[0]) - 'x';
    if (value && coordinate >= 0 && coordinate < dimensions<V> && key[1] == '\0') {
        auto copy = *value;
        return scalar(state, component(copy, coordinate));
    }
    luaL_getmetatable(state, metatable<V>());
    lua_getfield(state, -1, "methods");
    lua_pushvalue(state, 2);
    lua_gettable(state, -2);
    return 1;
}
template<class V> int newindex(lua_State* state) {
    const char* key = lua_tostring(state, 2);
    auto* value = test<V>(state, 1);
    if (key && value) {
        const float number = static_cast<float>(lua_tonumber(state, 3));
        const int coordinate = static_cast<unsigned char>(key[0]) - 'x';
        if (coordinate >= 0 && coordinate < dimensions<V> && key[1] == '\0')
            component(*value, coordinate) = number;
    }
    return 0;
}
template<class V> V arithmetic_argument(lua_State* state, int index) {
    if (const auto* value = test<V>(state, index)) return *value;
    if (lua_type(state, index) != LUA_TNUMBER)
        luaL_error(state, "vector arithmetic expects vectors or numbers");
    const float number = static_cast<float>(lua_tonumber(state, index));
    V value{};
    for (int i = 0; i < dimensions<V>; ++i) component(value, i) = number;
    return value;
}
template<class V, int Operation> int arithmetic(lua_State* state) {
    auto a = arithmetic_argument<V>(state, 1);
    V b{};
    if constexpr (Operation != 4) b = arithmetic_argument<V>(state, 2);
    for (int i = 0; i < dimensions<V>; ++i) {
        float& left = component(a, i);
        const float right = component(b, i);
        if constexpr (Operation == 0) left += right;
        else if constexpr (Operation == 1) left -= right;
        else if constexpr (Operation == 2) left *= right;
        else if constexpr (Operation == 3) left = right == 0.0f ? 0.0f : left / right;
        else left = -left;
    }
    return result(state, a);
}
template<class V> int equal(lua_State* state) {
    const auto* a = test<V>(state, 1);
    const auto* b = test<V>(state, 2);
    bool same = a && b;
    if (same) {
        same = a->x == b->x && a->y == b->y;
        if constexpr (dimensions<V> == 3) same = same && a->z == b->z;
    }
    lua_pushboolean(state, same);
    return 1;
}
template<class V> int tostring(lua_State* state) {
    const auto v = *checked<V>(state, 1);
    char output[dimensions<V> == 3 ? 96 : 64]{};
    if constexpr (dimensions<V> == 3) std::snprintf(output, sizeof(output), "%.2f %.2f %.2f", v.x, v.y, v.z);
    else std::snprintf(output, sizeof(output), "%.2f %.2f", v.x, v.y);
    lua_pushstring(state, output);
    return 1;
}
int is_zero(lua_State* state) {
    const auto v = *checked<Vec3>(state, 1);
    lua_pushboolean(state, v.x == 0.0f && v.y == 0.0f && v.z == 0.0f);
    return 1;
}
int is_valid(lua_State* state) {
    const auto v = *checked<Vec3>(state, 1);
    lua_pushboolean(state, std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z));
    return 1;
}
int cross(lua_State* state) {
    const auto a = *checked<Vec3>(state, 1), b = *checked<Vec3>(state, 2);
    return result(state, Vec3{a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x});
}
template<bool InPlace> int normalize(lua_State* state) {
    auto* original = checked<Vec3>(state, 1);
    const auto v = *original;
    const float square = square_length(v);
    float magnitude{};
    if constexpr (InPlace) magnitude = std::sqrt(square);
    Vec3 normalized{};
    if (square > 0.0f) {
        if constexpr (!InPlace) magnitude = std::sqrt(square);
        normalized = {v.x / magnitude, v.y / magnitude, v.z / magnitude};
        if constexpr (InPlace) *original = normalized;
    }
    if constexpr (InPlace) return scalar(state, magnitude);
    else return result(state, normalized);
}
template<int Operation> int rounded(lua_State* state) {
    auto v = *checked<Vec2>(state, 1);
    for (int i = 0; i < 2; ++i) {
        auto& value = component(v, i);
        if constexpr (Operation == 0) value = std::floor(value);
        else if constexpr (Operation == 1) value = std::ceil(value);
        else if ((std::bit_cast<std::uint32_t>(value) & 0x7fffffffU) < 0x4b000000U)
            value = std::round(value); // Original returns large/nonfinite bits unchanged.
    }
    return result(state, v);
}
int angle_vectors(lua_State* state) {
    const auto v = *checked<Vec3>(state, 1);
    const float pitch = v.x * degrees_to_radians, yaw = v.y * degrees_to_radians, roll = v.z * degrees_to_radians;
    const float sp = std::sin(pitch), cp = std::cos(pitch);
    const float sy = std::sin(yaw), cy = std::cos(yaw);
    const float sr = std::sin(roll), cr = std::cos(roll);
    const float negative_sr = -sr;
    const float right_scale = negative_sr * sp;
    const float up_scale = sp * cr;
    push_vec3(state, {cp * cy, cp * sy, -sp});
    push_vec3(state, {right_scale * cy + cr * sy, right_scale * sy + cr * -cy, cp * negative_sr});
    push_vec3(state, {up_scale * cy + sr * sy, up_scale * sy + sr * -cy, cp * cr});
    return 3;
}
int get_forward(lua_State* state) {
    lua_settop(state, 1);
    angle_vectors(state);
    lua_pop(state, 2);
    return 1;
}
int lua_vector_angles(lua_State* state) { return result(state, vector_angles(check_vec3(state, 1))); }
int lua_calc_angle(lua_State* state) {
    const auto source = check_vec3(state, 1), destination = check_vec3(state, 2);
    return result(state, calc_angle(source, destination));
}
int lua_angle_normalize(lua_State* state) {
    return scalar(state, angle_normalize(static_cast<float>(luaL_checknumber(state, 1))));
}
int lua_approach_angles(lua_State* state) {
    const auto current = check_vec3(state, 1), target = check_vec3(state, 2);
    return result(state, approach_angles(current, target, optional_number(state, 3, 10.0f)));
}
int lua_edge_point(lua_State* state) {
    const auto low = check_vec3(state, 1), high = check_vec3(state, 2), direction = check_vec3(state, 3);
    return result(state, edge_point(low, high, direction, optional_number(state, 4)));
}
int make_vec2(lua_State* state) {
    const float y = optional_number(state, lua_type(state, 2) > LUA_TNIL ? 2 : 1);
    const float x = optional_number(state, 1);
    return result(state, Vec2{x,y});
}
int make_vec3(lua_State* state) {
    const float z = optional_number(state, 3), y = optional_number(state, 2), x = optional_number(state, 1);
    return result(state, Vec3{x,y,z});
}
template<class V> void register_vector(lua_State* state, const luaL_Reg* methods) {
    if (luaL_newmetatable(state, metatable<V>())) {
        lua_newtable(state);
        luaL_setfuncs(state, methods, 0);
        lua_setfield(state, -2, "methods");
        const luaL_Reg meta[] = {
            {"__index", index<V>}, {"__newindex", newindex<V>},
            {"__add", arithmetic<V,0>}, {"__sub", arithmetic<V,1>},
            {"__mul", arithmetic<V,2>}, {"__div", arithmetic<V,3>},
            {"__unm", arithmetic<V,4>}, {"__eq", equal<V>}, {"__tostring", tostring<V>}, {nullptr,nullptr}};
        luaL_setfuncs(state, meta, 0);
        lua_pushstring(state, dimensions<V> == 3 ? "protected vector" : "protected vec2");
        lua_setfield(state, -2, "__metatable");
    }
    lua_pop(state, 1);
}
}

Vec2 check_vec2(lua_State* state, int index) { return *checked<Vec2>(state, index); }
Vec3 check_vec3(lua_State* state, int index) { return *checked<Vec3>(state, index); }
void push_vec2(lua_State* state, Vec2 value) { push(state, value); }
void push_vec3(lua_State* state, Vec3 value) { push(state, value); }

float angle_normalize(float value) noexcept {
    const float shifted = value + 180.0f;
    float remainder = std::fmod(shifted, 360.0f);
    if (remainder < 0.0f) remainder += 360.0f;
    return remainder + -180.0f;
}
Vec3 calc_angle(Vec3 source, Vec3 destination) noexcept {
    const Vec3 difference{destination.x-source.x, destination.y-source.y, destination.z-source.z};
    const float square = difference.x*difference.x + difference.y*difference.y;
    const float horizontal = square != 0.0f ? std::sqrt(square) : 0.0f;
    const float pitch = std::atan2(difference.z, horizontal) * -radians_to_degrees;
    const float yaw = std::atan2(difference.y, difference.x) * radians_to_degrees;
    return {wrap_once(pitch), wrap_once(yaw), 0.0f};
}
Vec3 vector_angles(Vec3 direction) noexcept {
    const float horizontal = std::sqrt(direction.x*direction.x + direction.y*direction.y);
    float pitch{}, yaw{};
    if (horizontal > minimum_speed) {
        pitch = (std::atan2(-direction.z, horizontal) * 180.0f) / pi;
        yaw = (std::atan2(direction.y, direction.x) * 180.0f) / pi;
    } else pitch = direction.z > 0.0f ? 270.0f : 90.0f;
    return {wrap_once(pitch), wrap_once(yaw), 0.0f};
}
Vec3 approach_angles(Vec3 current, Vec3 target, float speed) noexcept {
    speed = maxss(minimum_speed, speed);
    for (int i = 2; i >= 0; --i) {
        float& value = component(current, i);
        const float difference = component(target, i) - value;
        const float shifted = difference + 540.0f;
        const float delta = std::fmod(shifted, 360.0f) + -180.0f;
        const float limited = minss(speed, delta);
        value = maxss(-speed, limited) + value;
    }
    return current;
}
Vec3 edge_point(Vec3 low, Vec3 high, Vec3 direction, float inset) noexcept {
    Vec3 value{};
    for (int i = 0; i < 3; ++i) {
        const float d = component(direction, i);
        component(value, i) = d > 0.0f ? component(high, i) : d < 0.0f ? component(low, i) :
            (component(low, i) + component(high, i)) * 0.5f;
    }
    if (inset > 0.0f) {
        value.x += direction.x > 0.0f ? -inset : inset;
        value.y += direction.y > 0.0f ? -inset : inset;
    }
    return value;
}
void register_math_extensions(lua_State* state) {
    static const luaL_Reg vec3[] = {
        {"IsZero",is_zero}, {"Clone",clone<Vec3>}, {"Dist",distance<Vec3,false>}, {"DistSqr",distance<Vec3,true>},
        {"Dist2d",distance<Vec3,false,true>}, {"Dist2dSqr",distance<Vec3,true,true>}, {"Cross",cross},
        {"IsValid",is_valid}, {"Length",length<Vec3,false>}, {"LengthSqr",length<Vec3,true>},
        {"Length2d",length<Vec3,false,true>}, {"Length2dSqr",length<Vec3,true,true>}, {"Dot",dot<Vec3>},
        {"AngleVectors",angle_vectors}, {"VectorAngles",lua_vector_angles}, {"GetForward",get_forward},
        {"Normalize",normalize<true>}, {"Normalized",normalize<false>}, {nullptr,nullptr}};
    static const luaL_Reg vec2[] = {
        {"Clone",clone<Vec2>}, {"Floor",rounded<0>}, {"Ceil",rounded<1>}, {"Round",rounded<2>},
        {"Len",length<Vec2,false>}, {"LenSqr",length<Vec2,true>}, {"Dist",distance<Vec2,false>},
        {"DistSqr",distance<Vec2,true>}, {"Dot",dot<Vec2>}, {nullptr,nullptr}};
    register_vector<Vec3>(state, vec3);
    register_vector<Vec2>(state, vec2);
    lua_getglobal(state, "math");
    if (!lua_istable(state, -1)) luaL_error(state, "math table unavailable");
    const luaL_Reg methods[] = {
        {"CalcAngle",lua_calc_angle}, {"AngleNormalize",lua_angle_normalize}, {"ApproachAngles",lua_approach_angles},
        {"EdgePoint",lua_edge_point}, {"VectorAngles",lua_vector_angles}, {"Vec2",make_vec2},
        {"Vec3",make_vec3}, {"Vector",make_vec3}, {nullptr,nullptr}};
    luaL_setfuncs(state, methods, 0);
    lua_pop(state, 1);
}
}
