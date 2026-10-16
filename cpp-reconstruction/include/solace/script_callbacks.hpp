#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

struct lua_State;

namespace solace::script {
enum class Queue : std::uint32_t { present, create_move, frame_stage, event, input, menu_toggled };
using ProtectedCall = std::function<int(lua_State*, int nargs, int nresults)>;

struct DispatchResult {
    std::size_t invoked{}; // Includes a callback that returned a Lua error.
    int lua_status{};     // LUA_OK or the actual protected-call status.
    std::string error;
};

// One Lua state/script owner; serialize all use on the state's owning thread.
// This is an explicit host boundary, not an automatic source of game events.
class CallbackQueues final {
public:
    CallbackQueues(lua_State*, std::uint32_t owner_id, ProtectedCall);
    ~CallbackQueues();
    CallbackQueues(const CallbackQueues&) = delete;
    CallbackQueues& operator=(const CallbackQueues&) = delete;

    // Copies the top nargs arguments for each callback, discards callback results,
    // and consumes the original arguments, also for empty/inactive queues.
    // On Lua error: deactivate this owner, stop its queue, return the error.
    // Invalid queue/argument counts or a closed owner throw without touching the stack.
    DispatchResult dispatch(Queue, int nargs = 0);
    bool active() const noexcept;
    bool closed() const noexcept;
    std::uint32_t owner_id() const noexcept;
    std::size_t size(Queue) const;
    void deactivate() noexcept;
    // Call before lua_close. An internal Lua GC anchor also invalidates late C++
    // handles if the Lua state was closed first. Repeated close is harmless.
    void close() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::string_view queue_name(Queue);
// Names recovered from 0x18026D940; enumeration does not install host listeners.
std::span<const std::string_view> registered_event_names() noexcept;
}
