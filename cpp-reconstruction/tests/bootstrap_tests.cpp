#include <solace/bootstrap.hpp>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace solace::bootstrap;
constexpr FrameResult original_result = 0x8877665544332211ULL;
constexpr Entity controller = 0x100;
constexpr Entity pawn = 0x200;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Fixture final : Host {
    std::uint8_t loading = 0;
    std::uint8_t ready = 1;
    Entity outer = 0x9000;
    Entity inner = 0x10000;
    Context context{controller, pawn, 0};
    std::map<std::uint32_t, Entity> entities;
    std::map<std::uint32_t, Entity> cache;
    std::vector<std::pair<Entity, std::uint32_t>> adds;
    std::vector<std::uint32_t> lookups;
    std::vector<std::uint32_t> stages;
    std::vector<std::string> events;
    std::function<void()> original_effect;
    std::function<void()> context_effect;
    std::function<void(std::uint32_t)> lookup_effect;
    std::function<void()> add_effect;
    bool reject_adds = false;
    bool throw_context = false;
    bool throw_membership = false;
    int lock_depth = 0;
    int lock_acquires = 0;
    int lock_releases = 0;
    int inner_reads = 0;
    int membership_calls = 0;

    void seed(std::uint32_t index, Entity entity = 0) {
        entities[index] = entity != 0 ? entity : static_cast<Entity>(index) * 0x100;
    }
    FrameResult original_frame(std::uint32_t stage) override {
        stages.push_back(stage);
        events.emplace_back("original");
        if (original_effect) original_effect();
        return original_result;
    }
    std::uint8_t loading_flags() override { events.emplace_back("loading"); return loading; }
    std::uint8_t frame_ready_flags() override { events.emplace_back("ready"); return ready; }
    Entity entity_system_slot() override { return outer; }
    Entity entity_system_value(Entity slot) override {
        require(slot == outer, "system slot argument changed");
        ++inner_reads;
        return inner;
    }
    void acquire_context_shared() override { ++lock_acquires; ++lock_depth; }
    Context read_context_locked() override {
        require(lock_depth == 1, "context read must hold shared lock");
        if (throw_context) throw std::runtime_error("context failure");
        if (context_effect) context_effect();
        return context;
    }
    void release_context_shared() noexcept override { ++lock_releases; --lock_depth; }
    Entity lookup_entity(std::uint32_t index) override {
        require(lock_depth == 0, "enumeration retained context lock");
        lookups.push_back(index);
        if (lookup_effect) lookup_effect(index);
        // Models the existing host lookup's index-zero exclusion, not a new
        // condition in the Bootstrap implementation.
        if (index == 0) return 0;
        const auto it = entities.find(index);
        return it == entities.end() ? 0 : it->second;
    }
    void add_entity(Entity entity, std::uint32_t index) override {
        require(lock_depth == 0, "registration retained context lock");
        adds.emplace_back(entity, index);
        if (add_effect) add_effect();
        if (!reject_adds) cache[index] = entity;
    }
    bool contains(Entity value) override {
        require(lock_depth == 0, "membership retained context lock");
        ++membership_calls;
        if (throw_membership) throw std::runtime_error("membership failure");
        return std::any_of(cache.begin(), cache.end(), [value](const auto& item) {
            return item.second == value;
        });
    }
};

void invoke(Bootstrap& bootstrap, Fixture& host, std::uint32_t stage = 12) {
    const auto calls_before = host.stages.size();
    require(bootstrap.on_frame(host, stage) == original_result, "original return value changed");
    require(host.stages.size() == calls_before + 1, "original must run exactly once");
    require(host.stages.back() == stage, "original stage argument changed");
    require(host.lock_depth == 0 && host.lock_acquires == host.lock_releases,
            "shared context lock not balanced");
}

void waiting(const Bootstrap& bootstrap, const Fixture& host) {
    const auto d = bootstrap.diagnostics();
    require(d.status == Status::waiting && d.nonzero_entity_count == 0 &&
            d.enumerated_index_limit == 0 && d.controller == 0, "waiting diagnostics changed");
    require(host.adds.empty() && host.lookups.empty() && host.membership_calls == 0,
            "waiting frame consumed registration");
}

void complete(const Bootstrap& bootstrap, const Fixture& host, std::uint32_t count) {
    const auto d = bootstrap.diagnostics();
    require(d.status == Status::complete && d.nonzero_entity_count == count &&
            d.enumerated_index_limit == 0x4000 && d.controller == controller,
            "completed diagnostics do not match the scan");
    require(host.lookups.size() == 0x4000 && host.lookups.front() == 0 &&
            host.lookups.back() == 0x3fff && host.membership_calls == 1,
            "scan bounds or membership count changed");
}

void expect_throw(const std::function<void()>& body) {
    bool thrown = false;
    try { body(); } catch (const std::runtime_error&) { thrown = true; }
    require(thrown, "host exception was swallowed");
}

void non_stage12_waits() {
    Bootstrap b; Fixture h; h.seed(1);
    for (const auto stage : {0U, 3U, 11U, 13U}) invoke(b, h, stage);
    waiting(b, h);
    require(h.events.size() == 4, "non-target stage read readiness");
}
void loading_waits_then_ready() {
    Bootstrap b; Fixture h; h.seed(1); h.loading = 1;
    invoke(b, h); waiting(b, h);
    h.loading = 0; invoke(b, h); complete(b, h, 1);
}
void invalid_context_waits_then_ready() {
    Bootstrap b; Fixture h; h.seed(1); h.context = {};
    invoke(b, h); waiting(b, h);
    h.context = {controller, pawn, 0}; invoke(b, h); complete(b, h, 1);
}
void frame_ready_gate() {
    Bootstrap b; Fixture h; h.seed(1); h.ready = 0;
    invoke(b, h); waiting(b, h);
    h.ready = 1; invoke(b, h); complete(b, h, 1);
}
void null_entity_system_waits() {
    Bootstrap b; Fixture h; h.seed(1); h.inner = 0;
    invoke(b, h); waiting(b, h);
    require(h.lock_acquires == 0, "invalid system reached context");
    h.inner = 0x10000; invoke(b, h); complete(b, h, 1);
}
void sparse_table_chunk_boundaries_once() {
    Bootstrap b; Fixture h;
    const std::vector<std::uint32_t> indices{1, 7, 511, 512, 513, 1023, 4095, 16383};
    for (const auto index : indices) h.seed(index);
    h.entities[9] = 0;
    invoke(b, h); complete(b, h, static_cast<std::uint32_t>(indices.size()));
    for (std::size_t i = 0; i < indices.size(); ++i)
        require(h.adds.at(i) == std::pair<Entity, std::uint32_t>{indices[i] * 0x100ULL, indices[i]},
                "sparse scan order or entity/index argument changed");
    h.seed(14); invoke(b, h);
    require(h.adds.size() == indices.size() && h.lookups.size() == 0x4000,
            "completed bootstrap replayed");
}
void nonempty_cache_still_bootstraps() {
    Bootstrap b; Fixture h;
    for (const auto index : {1U, 3U, 512U}) h.seed(index);
    h.cache[1] = controller;
    invoke(b, h); complete(b, h, 3);
    require(h.adds.size() == 3 && h.cache.size() == 3, "preexisting cache skipped supplementation");
}
void original_frame_stage_runs_before_ready_check() {
    Bootstrap b; Fixture h; h.seed(1); h.ready = 0;
    h.original_effect = [&] { h.ready = 1; };
    invoke(b, h); complete(b, h, 1);
    require(h.events.front() == "original", "original did not run first");
}
void observer_can_supply_valid_context() {
    Bootstrap b; Fixture h; h.seed(1); h.context = {controller, 0, pawn};
    invoke(b, h); complete(b, h, 1);
}
void membership_false_nonempty_cache_is_status3() {
    Bootstrap b; Fixture h; h.seed(7);
    invoke(b, h); invoke(b, h);
    require(b.diagnostics().status == Status::missing_controller && h.adds.size() == 1 &&
            h.lookups.size() == 0x4000, "missing controller must be terminal failure");
}
void success_diagnostic_markers() {
    Bootstrap b; Fixture h;
    for (const auto index : {1U, 511U, 512U, 16383U}) h.seed(index);
    invoke(b, h); complete(b, h, 4);
}
void controller_required_even_with_pawn() {
    Bootstrap b; Fixture h; h.seed(1); h.context.controller = 0;
    invoke(b, h); waiting(b, h);
}
void pawn_or_observer_required_even_with_controller() {
    Bootstrap b; Fixture h; h.seed(1); h.context.pawn = 0;
    invoke(b, h); waiting(b, h);
}
void null_and_minus_one_system_pointer_guards() {
    Bootstrap b; Fixture h; h.seed(1);
    constexpr auto bad = std::numeric_limits<Entity>::max();
    for (const auto [outer, inner] : std::vector<std::pair<Entity, Entity>>{
            {0, 0x10000}, {bad, 0x10000}, {0x9000, 0}, {0x9000, bad}}) {
        h.outer = outer; h.inner = inner; invoke(b, h); waiting(b, h);
    }
    require(h.inner_reads == 2 && h.lock_acquires == 0, "invalid outer slot was dereferenced");
    h.outer = 0x9000; h.inner = 0x10000; invoke(b, h); complete(b, h, 1);
}
void index0_and_0x4000_excluded() {
    Bootstrap b; Fixture h; h.seed(0, 0x50); h.seed(16383); h.seed(16384);
    invoke(b, h);
    require(h.adds.size() == 1 && h.adds.front().second == 16383 && h.lookups.front() == 0 &&
            h.lookups.back() == 16383, "host lookup or exclusive upper boundary changed");
}
void on_add_rejection_reports_status3_once() {
    Bootstrap b; Fixture h; h.seed(1); h.seed(512); h.reject_adds = true;
    invoke(b, h); invoke(b, h);
    require(b.diagnostics().status == Status::missing_controller &&
            b.diagnostics().nonzero_entity_count == 2 && h.adds.size() == 2 && h.cache.empty(),
            "rejected adds were reported as successful or retried");
}
void in_progress_status_prevents_duplicate_start() {
    Bootstrap b; Fixture h; h.seed(1);
    h.lookup_effect = [&](std::uint32_t index) {
        if (index == 0) {
            require(b.diagnostics().status == Status::started, "scan not marked started");
            invoke(b, h);
            require(h.lookups.size() == 1, "reentrant call duplicated the scan");
        }
    };
    require(b.on_frame(h, 12) == original_result, "reentrant outer result changed");
    complete(b, h, 1);
    require(h.stages.size() == 2, "in-progress call skipped the original");
}

void readiness_masks_are_bitwise() {
    Bootstrap b; Fixture h; h.seed(1); h.loading = 2; h.ready = 3;
    invoke(b, h); complete(b, h, 1);
    Bootstrap other; Fixture blocked; blocked.seed(1); blocked.ready = 2;
    invoke(other, blocked); waiting(other, blocked);
}
void empty_scan_is_failure_without_replay() {
    Bootstrap b; Fixture h; invoke(b, h); h.seed(1); invoke(b, h);
    require(b.diagnostics().status == Status::missing_controller &&
            b.diagnostics().nonzero_entity_count == 0 &&
            b.diagnostics().enumerated_index_limit == 0x4000 && h.adds.empty() &&
            h.lookups.size() == 0x4000, "empty scan was successful or retried");
}
void captured_controller_is_used_after_context_changes() {
    Bootstrap b; Fixture h; h.seed(1);
    h.lookup_effect = [&](std::uint32_t index) { if (index == 0) h.context.controller = 0x999; };
    invoke(b, h); complete(b, h, 1);
}
void terminal_frames_skip_all_host_queries_after_original() {
    Bootstrap b; Fixture h; h.seed(1); invoke(b, h);
    h.events.clear(); invoke(b, h);
    require(h.events == std::vector<std::string>{"original"}, "terminal frame queried readiness");
}
void original_exception_propagates_without_consuming_state() {
    Bootstrap b; Fixture h;
    h.original_effect = [] { throw std::runtime_error("original failure"); };
    expect_throw([&] { b.on_frame(h, 12); }); waiting(b, h);
    require(h.stages.size() == 1 && h.events.size() == 1, "original failure continued workflow");
}
void context_exception_releases_shared_lock() {
    Bootstrap b; Fixture h; h.throw_context = true;
    expect_throw([&] { b.on_frame(h, 12); }); waiting(b, h);
    require(h.lock_depth == 0 && h.lock_releases == 1, "context exception leaked shared lock");
}
void add_exception_preserves_started_diagnostics_without_retry() {
    Bootstrap b; Fixture h; h.seed(1);
    h.add_effect = [] { throw std::runtime_error("add failure"); };
    expect_throw([&] { b.on_frame(h, 12); });
    const auto d = b.diagnostics();
    require(d.status == Status::started && d.nonzero_entity_count == 1 &&
            d.enumerated_index_limit == 0 && d.controller == controller,
            "partial registration was hidden by terminal success");
    invoke(b, h);
    require(h.adds.size() == 1 && h.lookups.size() == 2, "failed operation was retried");
}
void membership_exception_preserves_scan_markers() {
    Bootstrap b; Fixture h; h.seed(1); h.throw_membership = true;
    expect_throw([&] { b.on_frame(h, 12); });
    const auto d = b.diagnostics();
    require(d.status == Status::started && d.nonzero_entity_count == 1 &&
            d.enumerated_index_limit == 0x4000, "membership exception erased scan evidence");
    invoke(b, h); require(h.membership_calls == 1, "membership failure triggered replay");
}
void configuration_is_explicit_and_validated() {
    for (const Config config : {Config{12, 0, 1}, Config{12, 0x4001, 1}, Config{12, 1, 0}}) {
        bool threw = false;
        try { Bootstrap b(config); } catch (const std::invalid_argument&) { threw = true; }
        require(threw, "invalid configuration was silently accepted");
    }
    Bootstrap b({7, 2, 4}); Fixture h; h.seed(1); h.ready = 4;
    invoke(b, h, 12); waiting(b, h); invoke(b, h, 7);
    require(b.diagnostics().status == Status::complete && h.lookups.size() == 2 &&
            b.diagnostics().enumerated_index_limit == 2, "explicit configuration was ignored");
}

struct ConcurrentHost final : Host {
    static constexpr int workers = 8;
    std::barrier<> context_barrier{workers};
    std::atomic<int> originals{0};
    std::atomic<int> lookups{0};
    std::atomic<int> adds{0};
    std::atomic<int> memberships{0};
    std::atomic<int> acquisitions{0};
    std::atomic<int> releases{0};
    std::atomic<bool> registered{false};
    FrameResult original_frame(std::uint32_t) override { ++originals; return original_result; }
    std::uint8_t loading_flags() override { return 0; }
    std::uint8_t frame_ready_flags() override { return 1; }
    Entity entity_system_slot() override { return 1; }
    Entity entity_system_value(Entity) override { return 2; }
    void acquire_context_shared() override { ++acquisitions; }
    Context read_context_locked() override {
        // Every caller reaches the pre-CAS context read before any can start.
        context_barrier.arrive_and_wait();
        return {controller, pawn, 0};
    }
    void release_context_shared() noexcept override { ++releases; }
    Entity lookup_entity(std::uint32_t index) override { ++lookups; return index == 1 ? controller : 0; }
    void add_entity(Entity, std::uint32_t) override { ++adds; registered.store(true); }
    bool contains(Entity value) override { ++memberships; return value == controller && registered.load(); }
};

void competing_ready_frames_have_one_registration_owner() {
    Bootstrap b; ConcurrentHost h; std::atomic<int> wrong_results{0};
    std::vector<std::jthread> threads;
    for (int i = 0; i < ConcurrentHost::workers; ++i) {
        threads.emplace_back([&] { if (b.on_frame(h, 12) != original_result) ++wrong_results; });
    }
    threads.clear();
    require(wrong_results == 0 && h.originals == ConcurrentHost::workers && h.lookups == 0x4000 &&
            h.adds == 1 && h.memberships == 1 && h.acquisitions == ConcurrentHost::workers &&
            h.releases == ConcurrentHost::workers && b.diagnostics().status == Status::complete,
            "competing calls did not preserve original calls and one registration owner");
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void(*)()>> tests{
        {"non_stage12_waits", non_stage12_waits},
        {"loading_waits_then_ready", loading_waits_then_ready},
        {"invalid_context_waits_then_ready", invalid_context_waits_then_ready},
        {"frame_ready_gate", frame_ready_gate},
        {"null_entity_system_waits", null_entity_system_waits},
        {"sparse_table_chunk_boundaries_once", sparse_table_chunk_boundaries_once},
        {"nonempty_cache_still_bootstraps", nonempty_cache_still_bootstraps},
        {"original_frame_stage_runs_before_ready_check", original_frame_stage_runs_before_ready_check},
        {"observer_can_supply_valid_context", observer_can_supply_valid_context},
        {"membership_false_nonempty_cache_is_status3", membership_false_nonempty_cache_is_status3},
        {"success_diagnostic_markers", success_diagnostic_markers},
        {"controller_required_even_with_pawn", controller_required_even_with_pawn},
        {"pawn_or_observer_required_even_with_controller", pawn_or_observer_required_even_with_controller},
        {"null_and_minus_one_system_pointer_guards", null_and_minus_one_system_pointer_guards},
        {"index0_and_0x4000_excluded", index0_and_0x4000_excluded},
        {"on_add_rejection_reports_status3_once", on_add_rejection_reports_status3_once},
        {"in_progress_status_prevents_duplicate_start", in_progress_status_prevents_duplicate_start},
        {"readiness_masks_are_bitwise", readiness_masks_are_bitwise},
        {"empty_scan_is_failure_without_replay", empty_scan_is_failure_without_replay},
        {"captured_controller_is_used_after_context_changes", captured_controller_is_used_after_context_changes},
        {"terminal_frames_skip_all_host_queries_after_original", terminal_frames_skip_all_host_queries_after_original},
        {"original_exception_propagates_without_consuming_state", original_exception_propagates_without_consuming_state},
        {"context_exception_releases_shared_lock", context_exception_releases_shared_lock},
        {"add_exception_preserves_started_diagnostics_without_retry", add_exception_preserves_started_diagnostics_without_retry},
        {"membership_exception_preserves_scan_markers", membership_exception_preserves_scan_markers},
        {"configuration_is_explicit_and_validated", configuration_is_explicit_and_validated},
        {"competing_ready_frames_have_one_registration_owner", competing_ready_frames_have_one_registration_owner},
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    std::cout << (tests.size() - failures) << '/' << tests.size() << " bootstrap cases passed\n";
    return failures == 0 ? 0 : 1;
}
