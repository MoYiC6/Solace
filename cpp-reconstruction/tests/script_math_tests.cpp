#include <solace/script_math.hpp>
#include <lua.hpp>
#include <immintrin.h>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
using solace::script::Vec3;
std::size_t native_checks{};
void require(bool condition, const char* message) {
    ++native_checks;
    if (!condition) throw std::runtime_error(message);
}
bool same(float a, float b) {
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b) || (std::isnan(a) && std::isnan(b));
}
float scalar(__m128 value) { return _mm_cvtss_f32(value); }
// Instruction references transcribed from 0x18028A9E7..0x18028AA1B and
// 0x18028AB03..0x18028ABC6. Arithmetic, MINSS/MAXSS operand order and bit-select
// are independent of the implementation's scalar C++ branches. fmodf is the
// same mature CRT operation identified in original helper 0x180435B50.
float normalize_reference(float input) {
    __m128 x0 = _mm_add_ss(_mm_set_ss(input), _mm_set_ss(180.0f));
    x0 = _mm_set_ss(std::fmod(scalar(x0), 360.0f));
    __m128 x6 = _mm_add_ss(_mm_set_ss(360.0f), x0);
    __m128 x2 = _mm_cmplt_ss(x0, _mm_setzero_ps());
    x6 = _mm_and_ps(x6, x2);
    x2 = _mm_andnot_ps(x2, x0);
    x2 = _mm_or_ps(x2, x6);
    return scalar(_mm_add_ss(x2, _mm_set_ss(-180.0f)));
}
float approach_reference(float current, float target, float speed) {
    const __m128 limit = _mm_max_ss(_mm_set_ss(std::bit_cast<float>(0x3a83126fU)), _mm_set_ss(speed));
    __m128 delta = _mm_sub_ss(_mm_set_ss(target), _mm_set_ss(current));
    delta = _mm_add_ss(delta, _mm_set_ss(540.0f));
    delta = _mm_set_ss(std::fmod(scalar(delta), 360.0f));
    delta = _mm_add_ss(delta, _mm_set_ss(-180.0f));
    delta = _mm_min_ss(limit, delta);
    const auto sign = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int>(0x80000000U)));
    delta = _mm_max_ss(_mm_xor_ps(sign, limit), delta);
    return scalar(_mm_add_ss(delta, _mm_set_ss(current)));
}
void run(lua_State* state, const char* text) {
    if (luaL_loadstring(state, text) != LUA_OK || lua_pcall(state, 0, 0, 0) != LUA_OK) {
        const char* error = lua_tostring(state, -1);
        throw std::runtime_error(error ? error : "non-string Lua error");
    }
}
}

int main() {
    try {
        using namespace solace::script;
        std::unique_ptr<lua_State, decltype(&lua_close)> state(luaL_newstate(), lua_close);
        if (!state) throw std::bad_alloc{};
        luaL_openlibs(state.get());
        register_math_extensions(state.get());
        register_math_extensions(state.get());
        run(state.get(), R"lua(
checks=0
function check(v,m) checks=checks+1; assert(v,m) end
local V, V2 = math.Vec3, math.Vec2
local function near(a,b) return math.abs(a-b)<0.00002 end
local function vector(v,x,y,z) return near(v.x,x) and near(v.y,y) and (z==nil or near(v.z,z)) end
check(math.Vector==V and type(math.sin)=='function', 'aliases and standard math retained')
check(math.WorldToScreen==nil, 'host projection remains absent')
check(type(V())=='userdata' and type(V2())=='userdata','native vector userdata')
check(getmetatable(V())=='protected vector' and getmetatable(V2())=='protected vec2','protected metatables')
check(vector(V(),0,0,0) and vector(V(3),3,0,0),'Vec3 defaults')
check(vector(V('2','3','4'),2,3,4),'constructors accept numeric strings')
check(vector(V({},false,nil),0,0,0),'documented original constructor defaults')
check(vector(V2(3),3,3) and vector(V2(3,nil),3,3),'Vec2 scalar replication')
check(vector(V2(3,false),3,0) and vector(V2(3,{}),3,0),'Vec2 explicit nonnumeric second argument')
check(V(16777217).x==16777216,'float32 storage')
local v=V(1,2,3); v.x='4'; v.y={}; v.unknown=77
check(vector(v,4,0,3) and v.unknown==nil,'field writes and ignored unknown field')
v['z\0ignored']=8; check(v.z==8 and v['z\0ignored']==8,'C-string field key semantics')
check(v[{}]==nil and v[1]==nil, 'non-field indexes')
check(tostring(V(1.25,-2.5,3))=='1.25 -2.50 3.00','Vec3 formatting')
check(tostring(V2(1.25,-2.5))=='1.25 -2.50','Vec2 formatting')
check(V(1,2,3):Clone()==V(1,2,3) and V2(1,2):Clone()==V2(1,2),'clone and equality')
check(V(1,2,3)~=V(1,2,4) and V2(1,2)~=V2(1,3),'inequality')
check(V():IsZero() and not V(0,0,1):IsZero(), 'IsZero')
check(V(1,2,3):IsValid() and not V(math.huge):IsValid() and not V(0/0):IsValid(),'IsValid')
for _,ctor in ipairs({V,V2}) do
 local a=ctor(2,4,6); local b=ctor(1,2,3)
 check((a+b).x==3 and (a-b).y==2,'vector addition/subtraction')
 check((a*b).y==8 and (a/b).y==2,'component multiply/divide')
 check((a+2).x==4 and (2+a).y==6 and (10-a).x==8,'scalar left/right arithmetic')
 check((a*2).y==8 and (8/a).y==2 and (-a).x==-2,'scalar multiply/divide and unary')
 check((a/0).x==0 and (a/ctor(0,2,0)).y==2,'original zero-denominator result')
 check(not pcall(function() return a+'2' end),'arithmetic rejects numeric strings')
 check(not pcall(function() return a+{} end),'arithmetic rejects tables')
end
check(V(3,4,12):Length()==13 and V(3,4,12):LengthSqr()==169,'3D lengths')
check(V(3,4,12):Length2d()==5 and V(3,4,12):Length2dSqr()==25,'2D projection lengths')
check(V(4,6,15):Dist(V(1,2,3))==13 and V(4,6,15):DistSqr(V(1,2,3))==169,'3D distances')
check(V(4,6,15):Dist2d(V(1,2,3))==5 and V(4,6,15):Dist2dSqr(V(1,2,3))==25,'2D distances')
check(V(1,2,3):Dot(V(4,5,6))==32,'Vec3 dot')
check(V(1,0,0):Cross(V(0,1,0))==V(0,0,1),'cross orientation')
check(V2(3,4):Len()==5 and V2(3,4):LenSqr()==25,'Vec2 lengths')
check(V2(4,6):Dist(V2(1,2))==5 and V2(4,6):DistSqr(V2(1,2))==25,'Vec2 distances')
check(V2(1,2):Dot(V2(3,4))==11,'Vec2 dot')
check(V2(-1.2,2.7):Floor()==V2(-2,2),'floor')
check(V2(-1.2,2.7):Ceil()==V2(-1,3),'ceil')
check(V2(-1.5,2.5):Round()==V2(-2,3),'round halves away from zero')
v=V(3,4,0); local unit=v:Normalized()
check(vector(unit,0.6,0.8,0) and v==V(3,4,0),'Normalized returns new value')
check(v:Normalize()==5 and vector(v,0.6,0.8,0),'Normalize modifies and returns prior length')
check(V():Normalized():IsZero() and V():Normalize()==0,'zero normalization')
v=V(0/0,1,2); check(v:Normalized():IsZero(),'original NaN Normalized zero result')
local magnitude=v:Normalize();check(magnitude~=magnitude and v.x~=v.x and v.y==1,'NaN Normalize leaves original')
local forward,right,up=V():AngleVectors()
check(vector(forward,1,0,0) and vector(right,0,-1,0) and vector(up,0,0,1),'angle basis handedness')
check(select('#',V():AngleVectors())==3 and select('#',V():GetForward())==1,'basis return counts')
check(vector(V(0,90,0):GetForward(),0,1,0),'forward yaw')
check(vector(V(90,0,0):GetForward(),0,0,-1),'forward pitch')
check(vector(V(1,1,1):VectorAngles(),-35.26439,45,0),'vector method angles')
check(vector(math.VectorAngles(V(0,0,1)),-90,0,0),'up vector angles')
check(vector(math.VectorAngles(V(0,0,-1)),90,0,0),'down vector angles')
check(vector(math.VectorAngles(V()),90,0,0),'zero vector angles')
check(vector(math.VectorAngles(V(0.001,0,1)),-90,0,0),'horizontal threshold inclusive branch')
check(vector(math.VectorAngles(V(0/0,0,1)),-90,0,0),'unordered horizontal original branch')
check(vector(math.CalcAngle(V(),V(1,0,1)),-45,0,0),'CalcAngle pitch convention')
check(vector(math.CalcAngle(V(10,20,30),V(10,21,30)),0,90,0),'CalcAngle subtraction order')
check(vector(math.CalcAngle(V(),V()),0,0,0),'CalcAngle zero differs from VectorAngles')
check(math.AngleNormalize(180)==-180 and math.AngleNormalize(-180)==-180,'angle boundary')
check(math.AngleNormalize(721)==1 and math.AngleNormalize(-721)==-1,'angle full turns')
check(vector(math.ApproachAngles(V(170,-170,0),V(-170,170,90),5),175,-175,5),'approach shortest step')
check(vector(math.ApproachAngles(V(),V(30,30,30)),10,10,10),'approach default speed')
check(vector(math.ApproachAngles(V(),V(1,1,1),0),0.001,0.001,0.001),'approach minimum speed')
check(math.ApproachAngles(V(),V(-1000,0,0),1000).x==-280,'preserve negative remainder order')
check(vector(math.EdgePoint(V(0,10,20),V(8,18,28),V(1,-1,0)),8,10,24),'edge signs and midpoint')
check(vector(math.EdgePoint(V(0,10,20),V(8,18,28),V(1,-1,0),2),6,12,24),'edge inset only xy')
check(vector(math.EdgePoint(V(),V(8,8,8),V(),2),6,6,4),'zero direction midpoint plus xy inset')
check(vector(math.EdgePoint(V(),V(8,8,8),V(0/0,1,1)),4,8,8),'unordered direction midpoint')
check(not pcall(math.CalcAngle,{},V()) and not pcall(math.VectorAngles,V2()),'typed vector arguments')
check(not pcall(math.AngleNormalize,{}),'required scalar argument')
)lua");
        lua_getglobal(state.get(), "checks");
        const auto lua_checks = lua_tointeger(state.get(), -1);
        lua_pop(state.get(), 1);

        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        const std::array cases{0.0f,-0.0f,0.001f,-0.001f,180.0f,-180.0f,360.0f,-360.0f,
            540.0f,-540.0f,1000.125f,-1000.125f,16777216.0f,-16777216.0f,1e30f,-1e30f,nan,inf,-inf};
        for (const float input : cases) require(same(angle_normalize(input),normalize_reference(input)),"normalize SSE reference");
        for (const float current : cases) for (const float target : cases) for (const float speed : {0.0f,-1.0f,0.001f,10.0f,1000.0f,nan,inf}) {
            const auto result = approach_angles({current,current,current},{target,target,target},speed);
            const float expected = approach_reference(current,target,speed);
            require(same(result.x,expected) && same(result.y,expected) && same(result.z,expected),"approach SSE reference");
        }
        require(std::signbit(calc_angle({},{}).x),"CalcAngle preserves negative zero pitch");
        require(vector_angles({}).x==90.0f,"VectorAngles raw zero branch");
        push_vec2(state.get(), {7,9});
        require(check_vec2(state.get(),-1).y==9 && lua_rawlen(state.get(),-1)==8,"Vec2 shared layout");
        lua_pop(state.get(),1);
        push_vec3(state.get(), {7,9,11});
        require(check_vec3(state.get(),-1).z==11 && lua_rawlen(state.get(),-1)==12,"Vec3 shared layout");
        std::cout << "Passed " << lua_checks << " Lua math assertions and " << native_checks
                  << " native checks, including SSE instruction-reference comparisons.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Lua math test failure: " << error.what() << '\n';
        return 1;
    }
}
