#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <string_view>
#include <stdexcept>

struct lua_State;

namespace solace::script {
class Services;
class CallbackQueues;
struct RuntimeOptions {
    std::uint32_t owner_id = 1;
    unsigned timeout_ms = 2000;
    float initial_time = 0.0f;
};
// Only functions with recovered bodies are registered. Unimplemented host APIs
// are absent, rather than fake functions returning successful default values.
class Runtime final {
public:
    explicit Runtime(std::filesystem::path storage_root = {}, RuntimeOptions options = {});
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    void run(std::string_view source, const char* name = "reconstruction");
    void run_file(const std::filesystem::path& path);
    // Explicit host operations. Destruction does not invoke __shutdown; Lua GC
    // finalizers can still run. shutdown() invokes __shutdown once and reports errors.
    void shutdown();
    std::size_t advance_time(float absolute_seconds);
    std::size_t pending_tasks() const noexcept;
    std::vector<std::string> take_logs();
    CallbackQueues& callbacks() noexcept;
    lua_State* state() noexcept { return state_; }
private:
    lua_State* state_{};
    RuntimeOptions options_;
    std::unique_ptr<Services> services_;
    std::unique_ptr<CallbackQueues> callbacks_;
    bool shutdown_ = false;
};

float lerp(float first, float last, float amount) noexcept;
float clamp(float value, float bound1, float bound2) noexcept;
float remap(float value, float in_first, float in_last,
            float out_first, float out_last, bool clamped) noexcept;
std::uint64_t fnv1a(std::string_view bytes) noexcept;
}
