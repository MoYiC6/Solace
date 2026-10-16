#include <solace/assets.hpp>
#include <solace/config.hpp>
#include <solace/script.hpp>
#include <solace/script_session.hpp>
#include <solace/shaders.hpp>
#include <nlohmann/json.hpp>
#include <lua.hpp>
#include <bit>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <string>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

using json = nlohmann::json;
namespace {
std::string utf8_argument(std::wstring_view value) {
    if (value.empty()) return {};
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::length_error("Command argument exceeds Windows conversion limit");
    const int size = static_cast<int>(value.size());
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), size,
                                             nullptr, 0, nullptr, nullptr);
    if (required == 0) throw std::invalid_argument("Command argument contains invalid UTF-16");
    std::string output(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), size,
                            output.data(), required, nullptr, nullptr) != required)
        throw std::runtime_error("Command argument UTF-8 conversion failed");
    return output;
}
// Lua strings and exception text may contain arbitrary bytes. Preserve invalid
// UTF-8 explicitly as byte arrays instead of losing a whole JSON reply.
void text_field(json& object, const char* key, const std::string& value) {
    try {
        (void)json(value).dump();
        object[key] = value;
    } catch (const json::type_error& error) {
        if (error.id != 316) throw;
        object[key] = nullptr;
        object[std::string(key) + "_bytes"] = std::vector<unsigned char>(value.begin(), value.end());
    }
}
json reply(const std::string& operation, std::size_t line) {
    return {{"op", operation}, {"line", line}, {"ok", true}, {"results", json::object()},
            {"logs", json::array()}, {"errors", json::array()}};
}
void command_error(json& response, const std::string& message) {
    json error{{"scope", "command"}};
    text_field(error, "error", message);
    response["errors"].push_back(std::move(error));
    response["ok"] = false;
}
void add_report(json& response, const solace::script::SessionReport& report) {
    response["results"]["callbacks"] = report.callbacks;
    response["results"]["delayed_tasks"] = report.delayed_tasks;
    for (const auto& failure : report.failures) {
        json error{{"scope", "script"}, {"id", failure.id}};
        text_field(error, "file", failure.file);
        text_field(error, "error", failure.error);
        response["errors"].push_back(std::move(error));
        response["ok"] = false;
    }
}
void collect_session(json& response, solace::script::Session& session) {
    response["results"]["scripts"] = json::array();
    for (const auto& script : session.scripts()) {
        json entry{{"id", script.id}, {"delayed_tasks", script.delayed_tasks}};
        text_field(entry, "file", script.file);
        response["results"]["scripts"].push_back(std::move(entry));
    }
    for (const auto& log : session.take_logs()) {
        json entry{{"id", log.id}};
        text_field(entry, "file", log.file);
        text_field(entry, "text", log.text);
        response["logs"].push_back(std::move(entry));
    }
}
void emit_reply(const json& response) {
    std::cout << response.dump() << '\n' << std::flush;
    if (!std::cout) throw std::runtime_error("Failed writing JSON session reply");
}
json parse_command(const std::string& line) {
    std::vector<std::set<std::string>> keys;
    return json::parse(line, [&keys](int, json::parse_event_t event, json& value) {
        if (event == json::parse_event_t::object_start) keys.emplace_back();
        else if (event == json::parse_event_t::object_end) keys.pop_back();
        else if (event == json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
            throw std::invalid_argument("Duplicate JSON object field: " + value.get<std::string>());
        return true;
    });
}
void fields(const json& command, std::initializer_list<std::string_view> required) {
    for (const auto& [key, value] : command.items()) {
        (void)value;
        if (std::find(required.begin(), required.end(), key) == required.end())
            throw std::invalid_argument("Unknown command field: " + key);
    }
    for (const auto key : required)
        if (!command.contains(std::string(key))) throw std::invalid_argument("Missing command field: " + std::string(key));
}
std::string string_value(const json& value, const char* name) {
    if (!value.is_string()) throw std::invalid_argument(std::string(name) + " must be a string");
    return value.get<std::string>();
}
std::uint32_t script_id(const json& value) {
    if (!value.is_number_integer()) throw std::invalid_argument("id must be an integer from 1 to 4294967295");
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > 0 && number <= std::numeric_limits<std::uint32_t>::max()) return static_cast<std::uint32_t>(number);
    } else {
        const auto number = value.get<std::int64_t>();
        if (number > 0 && number <= std::numeric_limits<std::uint32_t>::max()) return static_cast<std::uint32_t>(number);
    }
    throw std::out_of_range("id must be an integer from 1 to 4294967295");
}
std::int64_t event_integer(const json& value) {
    if (!value.is_number_integer()) throw std::invalid_argument("Event arguments must be signed 64-bit integers");
    if (value.is_number_unsigned() && value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        throw std::out_of_range("Event integer exceeds signed 64-bit range");
    return value.get<std::int64_t>();
}
float session_time(const json& value) {
    if (!value.is_number()) throw std::invalid_argument("time must be a finite number representable as float32 seconds");
    const double number = value.get<double>();
    if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max())
        throw std::out_of_range("time exceeds finite float32 range");
    const float result = static_cast<float>(number);
    if (number != 0.0 && result == 0.0f) throw std::out_of_range("time underflows float32 range");
    return result;
}
solace::script::Queue dispatch_queue(const std::string& name) {
    using solace::script::Queue;
    if (name == "present") return Queue::present;
    if (name == "frame_stage") return Queue::frame_stage;
    if (name == "input") return Queue::input;
    if (name == "menu_toggled") return Queue::menu_toggled;
    throw std::invalid_argument("queue must be present, frame_stage, input or menu_toggled");
}
int script_session(const std::filesystem::path& root) {
    std::unique_ptr<solace::script::Session> session;
    try { session = std::make_unique<solace::script::Session>(root); }
    catch (const std::exception& error) {
        auto response = reply("startup", 0);
        command_error(response, error.what());
        emit_reply(response);
        return 1;
    }
    bool failed = false;
    std::size_t line_number = 0;
    std::string line;
    while (std::getline(std::cin, line)) {
        auto response = reply("invalid", ++line_number);
        bool closing = false;
        try {
            const auto command = parse_command(line);
            if (!command.is_object()) throw std::invalid_argument("Command must be a JSON object");
            if (!command.contains("op")) throw std::invalid_argument("Missing command field: op");
            const auto operation = string_value(command.at("op"), "op");
            response["op"] = operation;
            if (operation == "load") {
                fields(command, {"op", "file"});
                const auto file = string_value(command.at("file"), "file");
                if (file.empty() || file.find('\0') != std::string::npos)
                    throw std::invalid_argument("file must be a nonempty path without NUL bytes");
                const std::filesystem::path path{std::u8string(reinterpret_cast<const char8_t*>(file.data()), file.size())};
                if (path.has_root_path()) throw std::invalid_argument("file must be relative to the session root");
                response["results"]["id"] = session->load(path);
            } else if (operation == "reload" || operation == "unload") {
                fields(command, {"op", "id"});
                const auto id = script_id(command.at("id"));
                if (operation == "reload") {
                    response["results"]["previous_id"] = id;
                    response["results"]["id"] = session->reload(id);
                } else {
                    session->unload(id);
                    response["results"]["unloaded_id"] = id;
                }
            } else if (operation == "tick") {
                fields(command, {"op", "time"});
                const float time = session_time(command.at("time"));
                add_report(response, session->tick(time));
                response["results"]["time"] = time;
            } else if (operation == "dispatch") {
                fields(command, {"op", "queue", "args"});
                const auto queue = dispatch_queue(string_value(command.at("queue"), "queue"));
                const auto& arguments = command.at("args");
                if (!arguments.is_array()) throw std::invalid_argument("args must be an array");
                const std::size_t expected = queue == solace::script::Queue::present ? 0 : queue == solace::script::Queue::input ? 3 : 1;
                if (arguments.size() != expected) throw std::invalid_argument("Wrong event argument count");
                std::vector<solace::script::EventArgument> values;
                for (const auto& argument : arguments) {
                    if (queue == solace::script::Queue::menu_toggled) {
                        if (!argument.is_boolean()) throw std::invalid_argument("Menu event requires a boolean");
                        values.emplace_back(argument.get<bool>());
                    } else values.emplace_back(event_integer(argument));
                }
                add_report(response, session->dispatch(queue, values));
            } else if (operation == "list") {
                fields(command, {"op"});
            } else if (operation == "close") {
                fields(command, {"op"});
                closing = true;
                add_report(response, session->close());
            } else throw std::invalid_argument("Unknown session command: " + operation);
        } catch (const std::exception& error) { command_error(response, error.what()); }
        collect_session(response, *session);
        failed = failed || !response.at("ok").get<bool>();
        emit_reply(response);
        if (closing) return failed ? 1 : 0;
    }
    auto response = reply("close", line_number + 1);
    response["reason"] = std::cin.bad() ? "input_error" : "eof";
    if (std::cin.bad()) command_error(response, "Failed reading JSON session input");
    try { add_report(response, session->close()); }
    catch (const std::exception& error) { command_error(response, error.what()); }
    collect_session(response, *session);
    failed = failed || !response.at("ok").get<bool>();
    emit_reply(response);
    return failed ? 1 : 0;
}
int run_lua(const std::filesystem::path& file) {
    solace::script::Runtime runtime(std::filesystem::absolute(file).parent_path());
    std::vector<std::string> errors;
    try { runtime.run_file(file); }
    catch (const std::exception& error) { errors.push_back(std::string("run: ") + error.what()); }
    for (const auto& log : runtime.take_logs()) std::cout << log << '\n';
    try { runtime.shutdown(); }
    catch (const std::exception& error) { errors.push_back(std::string("shutdown: ") + error.what()); }
    for (const auto& log : runtime.take_logs()) std::cout << log << '\n';
    std::cout << std::flush;
    for (const auto& error : errors) std::cerr << "error: " << error << '\n';
    if (!std::cout) throw std::runtime_error("Failed writing Lua script logs");
    return errors.empty() ? 0 : 1;
}
int numeric_check() {
    solace::script::Runtime runtime;
    auto* state = runtime.state();
    std::string line;
    while (std::getline(std::cin, line)) {
        const auto request = json::parse(line);
        const auto op = request.at("op").get<std::string>();
        if (op == "Fnv1a") {
            const auto hex = request.at("hex").get<std::string>();
            if (hex.size() % 2) throw std::runtime_error("Odd hexadecimal string length");
            std::string bytes;
            const auto nibble = [](char c) -> unsigned {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                throw std::runtime_error("Invalid hexadecimal digit");
            };
            for (std::size_t i = 0; i < hex.size(); i += 2)
                bytes.push_back(static_cast<char>((nibble(hex[i]) << 4) | nibble(hex[i+1])));
            lua_getglobal(state, "utils"); lua_getfield(state, -1, "Fnv1a"); lua_remove(state, -2);
            lua_pushlstring(state, bytes.data(), bytes.size());
            if (lua_pcall(state, 1, 1, 0) != LUA_OK) throw std::runtime_error(lua_tostring(state, -1));
            std::cout << json{{"value", static_cast<std::uint64_t>(lua_tointeger(state, -1))}}.dump() << '\n';
            lua_pop(state, 1);
            continue;
        }
        const auto bits = request.at("bits").get<std::vector<std::uint32_t>>();
        const bool three = (op == "Lerp" || op == "Clamp") && bits.size() == 3;
        const bool five = (op == "RemapVal" || op == "RemapValClamped") && bits.size() == 5;
        if (!three && !five) throw std::runtime_error("Unknown numeric operation or wrong argument count");
        lua_getglobal(state, "math"); lua_getfield(state, -1, op.c_str()); lua_remove(state, -2);
        for (auto value : bits) lua_pushnumber(state, static_cast<double>(std::bit_cast<float>(value)));
        if (lua_pcall(state, static_cast<int>(bits.size()), 1, 0) != LUA_OK)
            throw std::runtime_error(lua_tostring(state, -1));
        const auto result = static_cast<float>(lua_tonumber(state, -1));
        lua_pop(state, 1);
        std::cout << json{{"bits", std::bit_cast<std::uint32_t>(result)}}.dump() << '\n';
    }
    return 0;
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 3 && std::wstring_view(argv[1]) == L"script-session") return script_session(argv[2]);
        if (argc == 2 && std::wstring_view(argv[1]) == L"numeric-check") return numeric_check();
        if (argc == 3 && std::wstring_view(argv[1]) == L"compile-shaders") {
            const auto report = solace::shaders::compile_all(argv[2]);
            std::cout << report.at("programs").size() << " shader stages compiled\n";
            return 0;
        }
        if (argc == 2 && std::wstring_view(argv[1]) == L"assets") {
            json entries = json::array();
            for (const auto& asset : solace::assets::all())
                entries.push_back({{"path", asset.path}, {"bytes", asset.bytes.size()}, {"sha256", asset.sha256}});
            std::cout << json{{"count", entries.size()}, {"total_bytes", solace::assets::total_bytes()},
                              {"assets", entries}}.dump(2) << '\n';
            return 0;
        }
        if (argc == 4 && std::wstring_view(argv[1]) == L"key") {
            std::cout << solace::config::key_string(solace::config::make_key(utf8_argument(argv[2]), utf8_argument(argv[3]))) << '\n';
            return 0;
        }
        if (argc == 3 && std::wstring_view(argv[1]) == L"inspect-config") {
            const auto document = solace::config::ConfigDocument::load_file(argv[2]);
            std::cout << json{{"version", document.json().at("version")},
                              {"field_count", document.json().at("fields").size()}}.dump(2) << '\n';
            return 0;
        }
        if (argc == 4 && std::wstring_view(argv[1]) == L"roundtrip-config") {
            if (std::filesystem::exists(argv[3])) throw std::runtime_error("Output already exists");
            const auto document = solace::config::ConfigDocument::load_file(argv[2]);
            document.save_file(argv[3]);
            const auto reread = solace::config::ConfigDocument::load_file(argv[3]);
            if (document.json() != reread.json()) throw std::runtime_error("Configuration roundtrip mismatch");
            std::cout << "Configuration roundtrip passed\n";
            return 0;
        }
        if (argc == 3 && std::wstring_view(argv[1]) == L"run-lua") {
            return run_lua(argv[2]);
        }
        std::cout << "Solace C++ reconstruction: independent recovered core, full application incomplete.\n"
                     "Commands: assets | key CATEGORY NAME | inspect-config FILE | "
                     "roundtrip-config INPUT OUTPUT | run-lua FILE | script-session ROOT | compile-shaders OUTPUT_DIR | numeric-check\n";
        return argc == 1 ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
