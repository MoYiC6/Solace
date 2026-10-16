#include <solace/script.hpp>
#include <solace/script_utils.hpp>
#include <solace/script_services.hpp>
#include <solace/script_callbacks.hpp>
#include <solace/script_math.hpp>
#include <solace/script_draw_values.hpp>
#include <lua.hpp>
#include <climits>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

namespace solace::script {
namespace {
// MINSS/MAXSS return their second operand for equal/unordered inputs. This is
// deliberately not std::fmin/fmax, whose NaN and signed-zero rules differ.
float minss(float first, float second) noexcept { return first < second ? first : second; }
float maxss(float first, float second) noexcept { return first > second ? first : second; }
float number(lua_State* state, int index) { return static_cast<float>(luaL_checknumber(state, index)); }
int lua_lerp(lua_State* state) {
    const float first = number(state, 1), last = number(state, 2), amount = number(state, 3);
    lua_pushnumber(state, lerp(first, last, amount));
    return 1;
}
int lua_clamp(lua_State* state) {
    const float value = number(state, 1), bound1 = number(state, 2), bound2 = number(state, 3);
    lua_pushnumber(state, clamp(value, bound1, bound2));
    return 1;
}
template<bool Clamped> int lua_remap(lua_State* state) {
    const float value = number(state, 1), first = number(state, 2), last = number(state, 3);
    const float out_first = number(state, 4), out_last = number(state, 5);
    lua_pushnumber(state, remap(value, first, last, out_first, out_last, Clamped));
    return 1;
}
void erase_field(lua_State* state, const char* name) {
    lua_pushnil(state);
    lua_setfield(state, -2, name);
}
void check(lua_State* state, int status) {
    if (status == LUA_OK) return;
    const char* message = lua_tostring(state, -1);
    const std::string copy = message ? message : "Lua returned a non-string error";
    lua_pop(state, 1);
    throw std::runtime_error(copy);
}
int invoke_shutdown(lua_State* state) {
    // Global lookup itself can invoke _G.__index and raise a Lua error.
    // Protect lookup and invocation together, with no C++ RAII on this frame.
    lua_getglobal(state, "__shutdown");
    if (lua_isfunction(state, -1)) lua_call(state, 0, 0);
    else lua_pop(state, 1);
    return 0;
}
}

float lerp(float first, float last, float amount) noexcept {
    const float difference = last - first;
    const float scaled = difference * amount;
    return scaled + first;
}
float clamp(float value, float bound1, float bound2) noexcept {
    const float low = minss(bound2, bound1);
    const float high = maxss(bound2, bound1);
    const float candidate = maxss(low, value);
    return high < value ? high : candidate;
}
float remap(float value, float in_first, float in_last,
            float out_first, float out_last, bool clamped) noexcept {
    float amount = 0.0f;
    if (in_last != in_first) amount = (value - in_first) / (in_last - in_first);
    if (clamped) amount = 1.0f < amount ? 1.0f : maxss(0.0f, amount);
    return lerp(out_first, out_last, amount);
}
std::uint64_t fnv1a(std::string_view bytes) noexcept {
    std::uint64_t value = 0xcbf29ce484222325ULL;
    for (const unsigned char byte : bytes) value = (value ^ byte) * 0x100000001b3ULL;
    return value & 0x7fffffffffffffffULL;
}

Runtime::Runtime(std::filesystem::path storage_root, RuntimeOptions options)
    : state_(luaL_newstate()), options_(options) {
    if (!state_) throw std::bad_alloc{};
    const auto cleanup = [this](lua_State* state) {
        if (callbacks_) callbacks_->close();
        if (services_) services_->close();
        lua_close(state);
    };
    std::unique_ptr<lua_State, decltype(cleanup)> construction_guard(state_, cleanup);
    luaL_openlibs(state_);
    if (options_.timeout_ms == 0 || options_.timeout_ms > static_cast<unsigned>(INT_MAX))
        throw std::invalid_argument("Lua timeout must be a positive signed millisecond value");
    lua_pushinteger(state_, options_.owner_id);
    lua_setfield(state_, LUA_REGISTRYINDEX, "__lua_script_id");
    // Restrictions observed in the target loader. This is not a security sandbox.
    lua_pushnil(state_); lua_setglobal(state_, "io");
    lua_getglobal(state_, "os");
    for (const auto name : {"execute", "exit", "remove", "rename", "tmpname", "setlocale"})
        erase_field(state_, name);
    lua_pop(state_, 1);
    lua_getglobal(state_, "package");
    erase_field(state_, "loadlib");
    lua_pushliteral(state_, ""); lua_setfield(state_, -2, "cpath");
    const auto pattern_u8 = storage_root.empty() ? std::u8string{} :
        (std::filesystem::absolute(storage_root) / "?.lua").generic_u8string();
    lua_pushlstring(state_, reinterpret_cast<const char*>(pattern_u8.data()), pattern_u8.size());
    lua_setfield(state_, -2, "path");
    // Native searchers depend on unrecovered module-loader/ffi integration.
    lua_getfield(state_, -1, "searchers");
    lua_pushnil(state_); lua_rawseti(state_, -2, 3);
    lua_pushnil(state_); lua_rawseti(state_, -2, 4);
    lua_pop(state_, 2);
    lua_getglobal(state_, "math");
    const luaL_Reg math_methods[] = {
        {"Lerp", lua_lerp}, {"Clamp", lua_clamp},
        {"RemapVal", lua_remap<false>}, {"RemapValClamped", lua_remap<true>}, {nullptr, nullptr}};
    luaL_setfuncs(state_, math_methods, 0);
    lua_pop(state_, 1);
    register_utils(state_, storage_root);
    register_math_extensions(state_);
    register_draw_values(state_);
    services_ = std::make_unique<Services>(state_, options_.initial_time);
    callbacks_ = std::make_unique<CallbackQueues>(state_, options_.owner_id,
        [timeout = options_.timeout_ms](lua_State* state, int arguments, int results) {
            return protected_call_with_timeout(state, arguments, results, timeout);
        });
    construction_guard.release();
}
Runtime::~Runtime() {
    if (callbacks_) callbacks_->close();
    if (services_) services_->close();
    if (state_) lua_close(state_);
}
void Runtime::run(std::string_view source, const char* name) {
    if (shutdown_) throw std::runtime_error("Script runtime is shut down");
    check(state_, luaL_loadbufferx(state_, source.data(), source.size(), name, "t"));
    check(state_, protected_call_with_timeout(state_, 0, 0, options_.timeout_ms));
}
void Runtime::run_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read Lua script: " + path.string());
    const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad()) throw std::runtime_error("Lua script read failed");
    if (shutdown_) throw std::runtime_error("Script runtime is shut down");
    const auto name_u8 = path.filename().generic_u8string();
    const auto chunk = "@" + std::string(reinterpret_cast<const char*>(name_u8.data()), name_u8.size());
    // The original file loader uses mode=nullptr, accepting source and matching
    // Lua 5.4 bytecode. run() remains an explicit source-only API.
    check(state_, luaL_loadbufferx(state_, text.data(), text.size(), chunk.c_str(), nullptr));
    check(state_, protected_call_with_timeout(state_, 0, 0, options_.timeout_ms));
}
void Runtime::shutdown() {
    if (shutdown_) return;
    shutdown_ = true;
    lua_pushcfunction(state_, invoke_shutdown);
    const int status = protected_call_with_timeout(state_, 0, 0, options_.timeout_ms);
    callbacks_->close(); services_->close();
    check(state_, status);
}
std::size_t Runtime::advance_time(float time) {
    if (shutdown_) throw std::runtime_error("Script runtime is shut down");
    services_->set_time(time);
    return services_->dispatch_due(options_.timeout_ms);
}
std::size_t Runtime::pending_tasks() const noexcept { return services_->pending(); }
std::vector<std::string> Runtime::take_logs() { return services_->take_logs(); }
CallbackQueues& Runtime::callbacks() noexcept { return *callbacks_; }
}
