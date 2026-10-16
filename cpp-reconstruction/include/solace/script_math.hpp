#pragma once
struct lua_State;

namespace solace::script {
struct Vec2 { float x{}, y{}; };
struct Vec3 { float x{}, y{}, z{}; };
static_assert(sizeof(Vec2) == 8 && sizeof(Vec3) == 12);

// These use the recovered lua_vec2/lua_vector full-userdata representation.
// check_* uses Lua's argument error convention; call before owning RAII locals.
Vec2 check_vec2(lua_State*, int index);
Vec3 check_vec3(lua_State*, int index);
void push_vec2(lua_State*, Vec2 value);
void push_vec3(lua_State*, Vec3 value);

// Registers both vector metatables and the eight recovered math names,
// preserving existing standard and previously recovered math functions.
void register_math_extensions(lua_State*);

float angle_normalize(float value) noexcept;
Vec3 calc_angle(Vec3 source, Vec3 destination) noexcept;
Vec3 approach_angles(Vec3 current, Vec3 target, float speed = 10.0f) noexcept;
Vec3 edge_point(Vec3 low, Vec3 high, Vec3 direction, float inset = 0.0f) noexcept;
Vec3 vector_angles(Vec3 direction) noexcept;
}
