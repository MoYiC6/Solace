#pragma once
#include <solace/script.hpp>
#include <solace/script_callbacks.hpp>
#include <map>
#include <span>
#include <variant>

namespace solace::script {
using EventArgument = std::variant<std::int64_t, double, bool, std::string>;
struct ScriptInfo { std::uint32_t id; std::string file; std::size_t delayed_tasks; };
struct ScriptFailure { std::uint32_t id; std::string file; std::string error; };
struct ScriptLog { std::uint32_t id; std::string file; std::string text; };
struct SessionReport {
    std::size_t callbacks = 0;
    std::size_t delayed_tasks = 0;
    std::vector<ScriptFailure> failures;
};

// Explicit standalone host. Every entry owns an independent Lua state, package
// cache, callback queues and delayed tasks. This does not install game hooks.
// All operations must be serialized on the owning thread.
class Session final {
public:
    explicit Session(std::filesystem::path root);
    ~Session(); // Lua GC can run; use unload/close for explicit __shutdown.
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    std::uint32_t load(const std::filesystem::path& relative_file);
    std::uint32_t reload(std::uint32_t id);
    void unload(std::uint32_t id);
    SessionReport close();
    // Due callbacks run before present callbacks for each script.
    SessionReport tick(float absolute_seconds);
    // Present=0 args, frame_stage=integer, input=3 integers, menu_toggled=bool.
    // Game-event and user-command queues require unavailable native host types.
    SessionReport dispatch(Queue, std::span<const EventArgument> arguments = {});
    std::vector<ScriptInfo> scripts() const;
    std::vector<ScriptLog> take_logs();
private:
    struct Entry {
        std::filesystem::path path;
        std::string file;
        std::unique_ptr<Runtime> runtime;
    };
    std::filesystem::path root_;
    std::map<std::uint32_t, Entry> scripts_;
    std::vector<ScriptLog> logs_;
    std::uint32_t next_id_ = 1;
    float time_ = 0;
    std::filesystem::path resolve(const std::filesystem::path&) const;
    void collect_logs(std::uint32_t, Entry&);
};
}
