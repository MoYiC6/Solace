#include <solace/script_session.hpp>
#include <lua.hpp>
#include <cmath>
#include <cwchar>
#include <limits>
#include <type_traits>
#include <utility>

namespace solace::script {
namespace {
std::string path_text(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
void validate_event(Queue queue, std::span<const EventArgument> arguments) {
    if (queue == Queue::create_move || queue == Queue::event)
        throw std::invalid_argument("This queue requires native game-host userdata");
    const std::size_t expected = queue == Queue::present ? 0 : queue == Queue::input ? 3 : 1;
    if (arguments.size() != expected) throw std::invalid_argument("Event argument count differs from recovered contract");
    for (const auto& argument : arguments) {
        if (queue == Queue::menu_toggled) {
            if (!std::holds_alternative<bool>(argument)) throw std::invalid_argument("Menu event requires a boolean");
        } else if (!std::holds_alternative<std::int64_t>(argument)) {
            throw std::invalid_argument("Frame and input events require integer arguments");
        }
    }
    queue_name(queue); // Also rejects invalid enum values when there are no args.
}
void push(lua_State* state, const EventArgument& argument) {
    std::visit([state](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, std::int64_t>) lua_pushinteger(state, value);
        else if constexpr (std::is_same_v<T, double>) lua_pushnumber(state, value);
        else if constexpr (std::is_same_v<T, bool>) lua_pushboolean(state, value);
        else lua_pushlstring(state, value.data(), value.size());
    }, argument);
}
}
Session::Session(std::filesystem::path root) {
    if (root.empty()) throw std::invalid_argument("Session requires an explicit script directory");
    root_ = std::filesystem::canonical(root);
    if (!std::filesystem::is_directory(root_)) throw std::invalid_argument("Script root is not a directory");
    // Lua's package.path uses semicolon separators and question-mark templates.
    // Reject ambiguous roots explicitly rather than silently searching elsewhere.
    if (root_.native().find_first_of(L";?") != std::wstring::npos)
        throw std::invalid_argument("Script root contains Lua path pattern syntax");
}
Session::~Session() = default;
std::filesystem::path Session::resolve(const std::filesystem::path& file) const {
    if (file.empty()) throw std::invalid_argument("Script file is empty");
    const auto resolved = std::filesystem::canonical(root_ / file);
    auto component = resolved.begin();
    for (const auto& parent : root_) {
        if (component == resolved.end() || _wcsicmp(parent.c_str(), component->c_str()) != 0)
            throw std::invalid_argument("Script path leaves the session directory");
        ++component;
    }
    if (!std::filesystem::is_regular_file(resolved)) throw std::invalid_argument("Script is not a regular file");
    return resolved;
}
void Session::collect_logs(std::uint32_t id, Entry& entry) {
    for (auto& line : entry.runtime->take_logs()) logs_.push_back({id, entry.file, std::move(line)});
}
std::uint32_t Session::load(const std::filesystem::path& file) {
    const auto path = resolve(file);
    for (const auto& [id, entry] : scripts_) {
        if (_wcsicmp(path.c_str(), entry.path.c_str()) == 0) return id;
        // A loaded source file may have been deleted since its state was created.
        // That must not prevent loading a different existing file. Preserve hard
        // link identity checks for entries whose source still exists.
        if (std::filesystem::exists(entry.path) && std::filesystem::equivalent(path, entry.path)) return id;
    }
    if (next_id_ == std::numeric_limits<std::uint32_t>::max()) throw std::overflow_error("Script ID space exhausted");
    const auto id = next_id_++;
    Entry entry{path, path_text(path.filename()), std::make_unique<Runtime>(root_, RuntimeOptions{id, 2000, time_})};
    try { entry.runtime->run_file(path); }
    catch (...) { collect_logs(id, entry); throw; }
    collect_logs(id, entry);
    scripts_.emplace(id, std::move(entry));
    return id;
}
void Session::unload(std::uint32_t id) {
    auto found = scripts_.find(id);
    if (found == scripts_.end()) throw std::out_of_range("Script ID is not loaded");
    // Remove ownership first: a failed shutdown cannot leave a half-live entry.
    auto entry = std::move(found->second);
    scripts_.erase(found);
    try { entry.runtime->shutdown(); }
    catch (...) { collect_logs(id, entry); throw; }
    collect_logs(id, entry);
}
std::uint32_t Session::reload(std::uint32_t id) {
    const auto found = scripts_.find(id);
    if (found == scripts_.end()) throw std::out_of_range("Script ID is not loaded");
    const auto path = found->second.path;
    unload(id);
    return load(path);
}
SessionReport Session::close() {
    SessionReport report;
    while (!scripts_.empty()) {
        const auto id = scripts_.begin()->first;
        const auto file = scripts_.begin()->second.file;
        try { unload(id); }
        catch (const std::exception& error) { report.failures.push_back({id, file, error.what()}); }
    }
    return report;
}
SessionReport Session::tick(float seconds) {
    if (!std::isfinite(seconds)) throw std::invalid_argument("Session time must be finite");
    time_ = seconds;
    SessionReport report;
    for (auto it = scripts_.begin(); it != scripts_.end();) {
        auto& [id, entry] = *it;
        bool failed = false;
        try {
            report.delayed_tasks += entry.runtime->advance_time(time_);
            const auto result = entry.runtime->callbacks().dispatch(Queue::present);
            report.callbacks += result.invoked;
            if (result.lua_status != LUA_OK) throw std::runtime_error(result.error);
        } catch (const std::exception& error) {
            failed = true;
            report.failures.push_back({id, entry.file, error.what()});
        }
        collect_logs(id, entry);
        if (failed) it = scripts_.erase(it); else ++it;
    }
    return report;
}
SessionReport Session::dispatch(Queue queue, std::span<const EventArgument> arguments) {
    validate_event(queue, arguments);
    SessionReport report;
    for (auto it = scripts_.begin(); it != scripts_.end();) {
        auto& [id, entry] = *it;
        auto* state = entry.runtime->state();
        const int top = lua_gettop(state);
        bool failed = false;
        try {
            if (!lua_checkstack(state, static_cast<int>(arguments.size()) + 1))
                throw std::runtime_error("Session event Lua stack exhausted");
            for (const auto& argument : arguments) push(state, argument);
            const auto result = entry.runtime->callbacks().dispatch(queue, static_cast<int>(arguments.size()));
            report.callbacks += result.invoked;
            if (result.lua_status != LUA_OK) throw std::runtime_error(result.error);
        } catch (const std::exception& error) {
            failed = true;
            report.failures.push_back({id, entry.file, error.what()});
        }
        lua_settop(state, top);
        collect_logs(id, entry);
        if (failed) it = scripts_.erase(it); else ++it;
    }
    return report;
}
std::vector<ScriptInfo> Session::scripts() const {
    std::vector<ScriptInfo> output;
    for (const auto& [id, entry] : scripts_)
        output.push_back({id, entry.file, entry.runtime->pending_tasks()});
    return output;
}
std::vector<ScriptLog> Session::take_logs() { return std::exchange(logs_, {}); }
}
