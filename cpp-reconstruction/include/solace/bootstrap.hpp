#pragma once

#include <atomic>
#include <cstdint>

namespace solace::bootstrap {

// Opaque values supplied by the host. This module never dereferences them.
using Entity = std::uint64_t;
using FrameResult = std::uint64_t;

enum class Status : std::uint32_t {
    waiting = 0,
    started = 1,
    complete = 2,
    missing_controller = 3,
};

struct Config {
    std::uint32_t ready_stage = 12;
    std::uint32_t entity_index_limit = 0x4000;
    std::uint8_t ready_mask = 1;
};

struct Context {
    Entity controller = 0;
    Entity pawn = 0;
    Entity observer_pawn = 0;
};

struct Diagnostics {
    Status status = Status::waiting;
    std::uint32_t nonzero_entity_count = 0;
    std::uint32_t enumerated_index_limit = 0;
    Entity controller = 0;
};

// The host owns object lifetime, synchronization, lookup semantics and storage.
// add_entity() is deliberately not interpreted as success: only contains()
// decides whether the captured local controller was actually registered.
class Host {
public:
    virtual ~Host() = default;

    virtual FrameResult original_frame(std::uint32_t stage) = 0;
    virtual std::uint8_t loading_flags() = 0;
    virtual std::uint8_t frame_ready_flags() = 0;
    virtual Entity entity_system_slot() = 0;
    virtual Entity entity_system_value(Entity valid_slot) = 0;

    virtual void acquire_context_shared() = 0;
    virtual Context read_context_locked() = 0;
    virtual void release_context_shared() noexcept = 0;

    // Called for indices [0, entity_index_limit). The original host lookup
    // rejects index zero; that rule belongs to the host, not the ASM wrapper.
    virtual Entity lookup_entity(std::uint32_t index) = 0;
    virtual void add_entity(Entity entity, std::uint32_t index) = 0;
    virtual bool contains(Entity controller) = 0;
};

class Bootstrap final {
public:
    explicit Bootstrap(Config config = {});

    // Calls original_frame exactly once before observing readiness. Exceptions
    // from the host propagate. A failed registration attempt is never retried.
    FrameResult on_frame(Host& host, std::uint32_t stage);

    // Terminal snapshots are published with acquire/release ordering. While
    // status==started, counters describe progress and may change concurrently.
    [[nodiscard]] Diagnostics diagnostics() const noexcept;
    [[nodiscard]] Config config() const noexcept { return config_; }

private:
    const Config config_;
    std::atomic<Status> status_{Status::waiting};
    std::atomic<std::uint32_t> nonzero_entity_count_{0};
    std::atomic<std::uint32_t> enumerated_index_limit_{0};
    std::atomic<Entity> controller_{0};
};

} // namespace solace::bootstrap
