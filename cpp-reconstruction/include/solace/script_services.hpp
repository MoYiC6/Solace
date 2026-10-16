#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
struct lua_State;

namespace solace::script {
// Lua status/error-on-stack contract. Default matches the recovered 2 s budget.
int protected_call(lua_State*, int argument_count, int result_count);
int protected_call_with_timeout(lua_State*, int argument_count, int result_count,
                                unsigned timeout_ms);

// Standalone host for recovered print, Delay and unpack. Caller supplies the
// float clock explicitly; no game clock or background dispatch is fabricated.
class Services final {
public:
    explicit Services(lua_State*, float initial_time = 0.0f);
    ~Services();
    Services(const Services&) = delete;
    Services& operator=(const Services&) = delete;
    void set_time(float seconds);
    std::size_t dispatch_due(unsigned timeout_ms = 2000);
    std::size_t pending() const noexcept;
    std::vector<std::string> take_logs();
    void close() noexcept; // Must precede lua_close; disables retained closures.
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
