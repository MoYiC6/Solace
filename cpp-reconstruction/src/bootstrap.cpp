#include <solace/bootstrap.hpp>

#include <limits>
#include <stdexcept>

namespace solace::bootstrap {
namespace {

bool valid_system_value(Entity value) noexcept {
    return value != 0 && value != std::numeric_limits<Entity>::max();
}

class SharedContextGuard final {
public:
    explicit SharedContextGuard(Host& host) : host_(host) {
        host_.acquire_context_shared();
    }
    ~SharedContextGuard() { host_.release_context_shared(); }
    SharedContextGuard(const SharedContextGuard&) = delete;
    SharedContextGuard& operator=(const SharedContextGuard&) = delete;

private:
    Host& host_;
};

} // namespace

Bootstrap::Bootstrap(Config config) : config_(config) {
    if (config.entity_index_limit == 0 || config.entity_index_limit > 0x4000) {
        throw std::invalid_argument("entity_index_limit must be in [1, 0x4000]");
    }
    if (config.ready_mask == 0) {
        throw std::invalid_argument("ready_mask must contain a readiness bit");
    }
}

FrameResult Bootstrap::on_frame(Host& host, std::uint32_t stage) {
    const FrameResult original_result = host.original_frame(stage);
    if (stage != config_.ready_stage ||
        status_.load(std::memory_order_acquire) != Status::waiting) {
        return original_result;
    }
    if ((host.loading_flags() & config_.ready_mask) != 0 ||
        (host.frame_ready_flags() & config_.ready_mask) == 0) {
        return original_result;
    }
    const auto system_slot = host.entity_system_slot();
    if (!valid_system_value(system_slot) ||
        !valid_system_value(host.entity_system_value(system_slot))) {
        return original_result;
    }

    Context context;
    {
        SharedContextGuard guard(host);
        context = host.read_context_locked();
    }
    if (context.controller == 0 || (context.pawn | context.observer_pawn) == 0) {
        return original_result;
    }

    auto expected = Status::waiting;
    if (!status_.compare_exchange_strong(expected, Status::started,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
        return original_result;
    }
    controller_.store(context.controller, std::memory_order_relaxed);
    for (std::uint32_t index = 0; index < config_.entity_index_limit; ++index) {
        const auto entity = host.lookup_entity(index);
        if (entity != 0) {
            nonzero_entity_count_.fetch_add(1, std::memory_order_relaxed);
            host.add_entity(entity, index);
        }
    }
    enumerated_index_limit_.store(config_.entity_index_limit, std::memory_order_relaxed);
    const auto final_status = host.contains(context.controller)
        ? Status::complete : Status::missing_controller;
    status_.store(final_status, std::memory_order_release);
    return original_result;
}

Diagnostics Bootstrap::diagnostics() const noexcept {
    Diagnostics result;
    result.status = status_.load(std::memory_order_acquire);
    result.nonzero_entity_count = nonzero_entity_count_.load(std::memory_order_relaxed);
    result.enumerated_index_limit = enumerated_index_limit_.load(std::memory_order_relaxed);
    result.controller = controller_.load(std::memory_order_relaxed);
    return result;
}

} // namespace solace::bootstrap
