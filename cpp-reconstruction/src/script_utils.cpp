#include <solace/script_utils.hpp>
#include <solace/script.hpp>
#include <lua.hpp>
#include <nlohmann/json.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <wincrypt.h>

#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace solace::script {
namespace {
using Json = nlohmann::json;
// Trivial userdata remains valid after a manually invoked __gc. The owned path
// can be released once without destroying the userdata object's lifetime.
struct Context { std::filesystem::path* root{}; };
constexpr const char* context_metatable = "solace.recovered.utils.context";

[[noreturn]] void win32_error(const char* operation) {
    throw std::runtime_error(std::string(operation) + ": Win32 error " + std::to_string(GetLastError()));
}

// Only call lua_error after the implementation and all C++ catch objects have
// left scope. Lua argument checks likewise happen before owning C++ locals.
template<int (*Function)(lua_State*)> int guarded(lua_State* state) {
    try { return Function(state); }
    catch (const std::exception& error) { lua_pushstring(state, error.what()); }
    catch (...) { lua_pushliteral(state, "utils: unknown C++ exception"); }
    return lua_error(state);
}

std::string_view checked_bytes(lua_State* state, int argument) {
    std::size_t length{};
    const char* data = luaL_checklstring(state, argument, &length);
    return {data, length};
}
int push_bytes(lua_State* state, std::string_view bytes) {
    lua_pushlstring(state, bytes.data(), bytes.size());
    return 1;
}
DWORD dword_size(std::size_t size) {
    if (size > std::numeric_limits<DWORD>::max()) throw std::length_error("utils input exceeds the Win32 length limit");
    return static_cast<DWORD>(size);
}
int int_size(std::size_t size) {
    if (size > INT_MAX) throw std::length_error("utils input exceeds the Win32 string length limit");
    return static_cast<int>(size);
}

std::wstring utf16(std::string_view text) {
    if (text.empty()) return {};
    const int size = int_size(text.size());
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), size, nullptr, 0);
    if (!length) win32_error("Convert UTF-8 path");
    std::wstring output(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, text.data(), size, output.data(), length) != length)
        win32_error("Convert UTF-8 path");
    return output;
}
std::string utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int size = int_size(text.size());
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), size, nullptr, 0, nullptr, nullptr);
    if (!length) win32_error("Convert UTF-16 text");
    std::string output(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, text.data(), size, output.data(), length, nullptr, nullptr) != length)
        win32_error("Convert UTF-16 text");
    return output;
}

std::filesystem::path root_path(lua_State* state) {
    const int index = lua_upvalueindex(1);
    const auto* context = static_cast<const Context*>(luaL_testudata(state, index, context_metatable));
    if (!context || lua_rawlen(state, index) != sizeof(Context))
        throw std::runtime_error("utils: invalid storage context");
    if (!context->root) throw std::runtime_error("utils: storage context is closed");
    if (context->root->empty()) throw std::runtime_error("lua folder unavailable");
    return *context->root;
}
std::filesystem::path resolve(lua_State* state, const char* argument) {
    const auto root = root_path(state);
    const std::string_view path(argument); // Original resolver uses strlen.
    for (unsigned char byte : path)
        if (byte == ':' || byte < 32) throw std::runtime_error("invalid file path");
    std::error_code error;
    const auto candidate = std::filesystem::weakly_canonical(root / utf16(path), error);
    if (error) throw std::runtime_error("invalid file path: " + error.message());
    const auto canonical_root = std::filesystem::weakly_canonical(root, error);
    if (error || canonical_root.empty()) throw std::runtime_error("invalid file path");
    auto child = candidate.begin();
    for (const auto& component : canonical_root) {
        if (child == candidate.end() || _wcsicmp(component.c_str(), child->c_str()) != 0)
            throw std::runtime_error("file path escapes the lua folder");
        ++child;
    }
    return candidate;
}

int base64_encode(lua_State* state) {
    const auto input = checked_bytes(state, 1);
    if (input.empty()) return push_bytes(state, "");
    DWORD length{};
    constexpr DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    const auto size = dword_size(input.size());
    const auto* data = reinterpret_cast<const BYTE*>(input.data());
    if (!CryptBinaryToStringA(data, size, flags, nullptr, &length)) win32_error("Base64 encode");
    std::string output(length, '\0');
    if (!CryptBinaryToStringA(data, size, flags, output.data(), &length)) win32_error("Base64 encode");
    output.resize(length); // CryptBinaryToString output count excludes NUL on success.
    return push_bytes(state, output);
}
int base64_decode(lua_State* state) {
    const auto input = checked_bytes(state, 1);
    std::string normalized;
    normalized.reserve(input.size());
    for (const unsigned char byte : input) {
        if (byte == 9 || byte == 10 || byte == 13 || byte == 32) continue;
        if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9') || byte == '+' || byte == '/') normalized.push_back(static_cast<char>(byte));
        else break;
    }
    // The original bit accumulator emits floor(6*n/8) bytes and ignores leftover
    // bits. Normalize that prefix to canonical padding before using Crypt32.
    if (normalized.size() % 4 == 1) normalized.pop_back();
    if (normalized.empty()) return push_bytes(state, "");
    while (normalized.size() % 4) normalized.push_back('=');
    DWORD length{};
    const auto size = dword_size(normalized.size());
    if (!CryptStringToBinaryA(normalized.data(), size, CRYPT_STRING_BASE64, nullptr, &length, nullptr, nullptr))
        win32_error("Base64 decode");
    std::string output(length, '\0');
    if (!CryptStringToBinaryA(normalized.data(), size, CRYPT_STRING_BASE64,
                             reinterpret_cast<BYTE*>(output.data()), &length, nullptr, nullptr))
        win32_error("Base64 decode");
    output.resize(length);
    return push_bytes(state, output);
}
int unix_time(lua_State* state) {
    FILETIME time{};
    GetSystemTimePreciseAsFileTime(&time);
    const std::uint64_t ticks = (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    lua_pushinteger(state, static_cast<lua_Integer>(ticks - 116444736000000000ULL) / 10000000);
    return 1;
}
int date(lua_State* state) {
    const __time64_t now = _time64(nullptr);
    std::tm local{};
    if (_localtime64_s(&local, &now) != 0) throw std::runtime_error("GetDate: local time conversion failed");
    char output[64]{};
    std::snprintf(output, sizeof(output), "%04d-%02d-%02d %02d:%02d:%02d",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
    return push_bytes(state, output);
}
int fnv(lua_State* state) {
    const auto input = checked_bytes(state, 1);
    lua_pushinteger(state, static_cast<lua_Integer>(fnv1a(input)));
    return 1;
}
int murmur(lua_State* state) {
    const auto input = checked_bytes(state, 1);
    // Recovered MurmurHash64A (Austin Appleby's public-domain MurmurHash2),
    // seed zero, with Solace's sign-bit mask after finalization.
    constexpr std::uint64_t multiplier = 0xc6a4a7935bd1e995ULL;
    std::uint64_t hash = input.size() * multiplier;
    std::size_t offset = 0;
    for (; input.size() - offset >= 8; offset += 8) {
        std::uint64_t block{};
        std::memcpy(&block, input.data() + offset, 8);
        block *= multiplier;
        block ^= block >> 47;
        block *= multiplier;
        hash = (hash ^ block) * multiplier;
    }
    for (std::size_t i = 0; i < input.size() - offset; ++i)
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(input[offset + i])) << (8 * i);
    if (offset != input.size()) hash *= multiplier;
    hash ^= hash >> 47;
    hash *= multiplier;
    hash ^= hash >> 47;
    lua_pushinteger(state, static_cast<lua_Integer>(hash & 0x7fffffffffffffffULL));
    return 1;
}

Json to_json(lua_State* state, int argument, unsigned depth) {
    const int index = lua_absindex(state, argument);
    switch (lua_type(state, index)) {
    case LUA_TBOOLEAN: return lua_toboolean(state, index) != 0;
    case LUA_TNUMBER:
        if (lua_isinteger(state, index)) return lua_tointeger(state, index);
        return lua_tonumber(state, index);
    case LUA_TSTRING: {
        std::size_t length{};
        const char* data = lua_tolstring(state, index, &length);
        return std::string(data, length);
    }
    case LUA_TTABLE: {
        if (depth >= 65) return nullptr;
        if (!lua_checkstack(state, 4)) throw std::runtime_error("json encode: nesting too deep");
        const auto length = lua_rawlen(state, index);
        std::size_t members{};
        lua_pushnil(state);
        while (lua_next(state, index)) { ++members; lua_pop(state, 1); }
        if (length > 0 && members == length) {
            auto array = Json::array();
            for (std::size_t i = 1; i <= length; ++i) {
                lua_rawgeti(state, index, static_cast<lua_Integer>(i));
                array.push_back(to_json(state, -1, depth + 1));
                lua_pop(state, 1);
            }
            return array;
        }
        auto object = Json::object();
        lua_pushnil(state);
        while (lua_next(state, index)) {
            auto value = to_json(state, -1, depth + 1);
            lua_pushvalue(state, -2);
            if (lua_type(state, -1) == LUA_TSTRING || lua_type(state, -1) == LUA_TNUMBER) {
                std::size_t length_key{};
                const char* key = lua_tolstring(state, -1, &length_key);
                object[std::string(key, length_key)] = std::move(value);
            }
            lua_pop(state, 2);
        }
        return object;
    }
    default: return nullptr;
    }
}
void push_json(lua_State* state, const Json& value) {
    if (!lua_checkstack(state, 3)) throw std::runtime_error("json decode: nesting too deep");
    if (value.is_null()) lua_pushnil(state);
    else if (value.is_boolean()) lua_pushboolean(state, value.get<bool>());
    else if (value.is_number_unsigned()) lua_pushinteger(state, std::bit_cast<std::int64_t>(value.get<std::uint64_t>()));
    else if (value.is_number_integer()) lua_pushinteger(state, value.get<std::int64_t>());
    else if (value.is_number_float()) lua_pushnumber(state, value.get<double>());
    else if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        lua_pushlstring(state, text.data(), text.size());
    } else if (value.is_array()) {
        lua_createtable(state, int_size(value.size()), 0);
        lua_Integer index = 1;
        for (const auto& item : value) { push_json(state, item); lua_rawseti(state, -2, index++); }
    } else if (value.is_object()) {
        lua_createtable(state, 0, int_size(value.size()));
        for (const auto& item : value.items()) {
            push_json(state, item.value());
            lua_setfield(state, -2, item.key().c_str()); // Original also truncates embedded NUL keys.
        }
    } else lua_pushnil(state);
}
int json_encode(lua_State* state) {
    const bool pretty = lua_toboolean(state, 2) != 0;
    return push_bytes(state, to_json(state, 1, 0).dump(pretty ? 2 : -1));
}
int json_decode(lua_State* state) {
    const auto input = checked_bytes(state, 1);
    const auto value = Json::parse(input.begin(), input.end(), nullptr, false);
    if (value.is_discarded()) {
        lua_pushnil(state); lua_pushliteral(state, "invalid json"); return 2;
    }
    push_json(state, value);
    return 1;
}

int file_exists(lua_State* state) {
    const char* argument = luaL_checkstring(state, 1);
    const auto path = resolve(state, argument);
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    lua_pushboolean(state, exists && !error);
    return 1;
}
int file_read(lua_State* state) {
    const char* argument = luaL_checkstring(state, 1);
    std::ifstream stream(resolve(state, argument), std::ios::binary);
    if (!stream) { lua_pushnil(state); return 1; }
    const std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    if (stream.bad()) throw std::runtime_error("FileRead: read failed");
    return push_bytes(state, text);
}
int file_write(lua_State* state) {
    const auto bytes = checked_bytes(state, 2);
    const char* argument = luaL_checkstring(state, 1);
    std::ofstream stream(resolve(state, argument), std::ios::binary | std::ios::trunc);
    if (stream) stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    // close() flushes buffered data and exposes both write and close failures.
    stream.close();
    lua_pushboolean(state, static_cast<bool>(stream));
    return 1;
}
int file_remove(lua_State* state) {
    const char* argument = luaL_checkstring(state, 1);
    const auto path = resolve(state, argument);
    std::error_code error;
    std::filesystem::remove(path, error);
    lua_pushboolean(state, !error); // Original checks error_code, not removed bool.
    return 1;
}
int file_rename(lua_State* state) {
    const char* old_arg = luaL_checkstring(state, 1);
    const char* new_arg = luaL_checkstring(state, 2);
    const auto new_path = resolve(state, new_arg);
    const auto old_path = resolve(state, old_arg);
    lua_pushboolean(state, MoveFileExW(old_path.c_str(), new_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED));
    return 1;
}
int file_enumerate(lua_State* state) {
    const char* argument = lua_isnoneornil(state, 1) ? "." : luaL_checkstring(state, 1);
    const auto path = resolve(state, argument);
    lua_newtable(state);
    lua_Integer index = 1;
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        const auto name = utf8(entry.path().filename().wstring());
        lua_pushlstring(state, name.data(), name.size());
        lua_rawseti(state, -2, index++);
    }
    return 1;
}
int file_create_directories(lua_State* state) {
    const char* argument = luaL_checkstring(state, 1);
    const auto path = resolve(state, argument);
    std::error_code error;
    std::filesystem::create_directories(path, error);
    lua_pushboolean(state, !error);
    return 1;
}

Json read_database(const std::filesystem::path& path) {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) throw std::runtime_error("Db: cannot inspect storage.json: " + error.message());
    if (!exists) return Json::object();
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Db: cannot read existing storage.json");
    auto database = Json::parse(input, nullptr, false);
    if (input.bad() || !database.is_object())
        throw std::runtime_error("Db: existing storage.json is invalid or not an object; original file preserved");
    return database;
}
int db_save(lua_State* state) {
    const char* key_arg = luaL_checkstring(state, 1);
    const std::string key(key_arg);
    const auto path = root_path(state) / L"storage.json";
    auto database = read_database(path);
    // Original JsonEncode -> JSON parse also normalizes NaN/Infinity to null.
    database[key] = Json::parse(to_json(state, 2, 0).dump());
    const auto serialized = database.dump();
    std::ofstream output(path, std::ios::trunc);
    if (output) output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
    output.close();
    lua_pushboolean(state, static_cast<bool>(output));
    return 1;
}
int db_load(lua_State* state) {
    const char* key_arg = luaL_checkstring(state, 1);
    const auto path = root_path(state) / L"storage.json";
    const auto database = read_database(path);
    const auto found = database.find(key_arg);
    if (found == database.end()) return 0;
    push_json(state, *found);
    return 1;
}

int clipboard_get(lua_State* state) {
    if (!OpenClipboard(nullptr)) win32_error("ClipboardGet: open");
    std::wstring text;
    try {
        const HANDLE data = GetClipboardData(CF_UNICODETEXT);
        if (!data) win32_error("ClipboardGet: Unicode text unavailable");
        const auto* locked = static_cast<const wchar_t*>(GlobalLock(data));
        if (!locked) win32_error("ClipboardGet: lock");
        try { text = locked; } catch (...) { GlobalUnlock(data); throw; }
        GlobalUnlock(data);
    } catch (...) { CloseClipboard(); throw; }
    if (!CloseClipboard()) win32_error("ClipboardGet: close");
    return push_bytes(state, utf8(text));
}
int clipboard_set(lua_State* state) {
    const auto input = checked_bytes(state, 1);
    const auto text = utf16(input);
    bool success = false;
    // OpenClipboard(nullptr) + EmptyClipboard makes SetClipboardData fail.
    // The system STATIC class supplies a valid, non-visible owner without a
    // custom window procedure or a class registration surviving this call.
    const HWND owner = CreateWindowExW(0, L"STATIC", nullptr, 0, 0, 0, 0, 0,
                                      HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (owner) {
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
        if (memory) {
            void* locked = GlobalLock(memory);
            if (locked) {
                std::memcpy(locked, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
                SetLastError(ERROR_SUCCESS);
                const bool unlocked = GlobalUnlock(memory) != FALSE || GetLastError() == ERROR_SUCCESS;
                // Prepare all data before clearing the previous clipboard.
                if (unlocked && OpenClipboard(owner)) {
                    if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory)) {
                        memory = nullptr; // The system owns the allocation now.
                        success = true;
                    }
                    if (!CloseClipboard()) success = false;
                }
            }
            if (memory && GlobalFree(memory) != nullptr) success = false;
        }
        if (!DestroyWindow(owner)) success = false;
    }
    lua_pushboolean(state, success);
    return 1;
}
int destroy_context(lua_State* state) {
    auto* context = static_cast<Context*>(luaL_testudata(state, 1, context_metatable));
    if (context && lua_rawlen(state, 1) == sizeof(Context)) {
        delete context->root;
        context->root = nullptr;
    }
    return 0;
}
} // namespace

void register_utils(lua_State* state, const std::filesystem::path& storage_root) {
    static const luaL_Reg methods[] = {
        {"Base64Encode", guarded<base64_encode>}, {"Base64Decode", guarded<base64_decode>},
        {"GetUnixTime", guarded<unix_time>}, {"GetDate", guarded<date>},
        {"Murmur2", guarded<murmur>}, {"Fnv1a", guarded<fnv>},
        {"ClipboardGet", guarded<clipboard_get>}, {"ClipboardSet", guarded<clipboard_set>},
        {"JsonEncode", guarded<json_encode>}, {"JsonDecode", guarded<json_decode>},
        {"FileExists", guarded<file_exists>}, {"FileRead", guarded<file_read>},
        {"FileWrite", guarded<file_write>}, {"FileRemove", guarded<file_remove>},
        {"FileRename", guarded<file_rename>}, {"FileEnumerate", guarded<file_enumerate>},
        {"FileCreateDirectories", guarded<file_create_directories>},
        {"DbSave", guarded<db_save>}, {"DbLoad", guarded<db_load>}, {nullptr, nullptr}
    };
    luaL_newlibtable(state, methods);
    auto* context_memory = static_cast<Context*>(lua_newuserdatauv(state, sizeof(Context), 0));
    new(context_memory) Context{};
    if (luaL_newmetatable(state, context_metatable)) {
        lua_pushcfunction(state, destroy_context);
        lua_setfield(state, -2, "__gc");
    }
    lua_setmetatable(state, -2);
    // Attach the finalizer before allocating the nontrivial object, so later
    // failures leave ownership with a finalizable Lua userdata.
    context_memory->root = new std::filesystem::path(storage_root);
    luaL_setfuncs(state, methods, 1);
    lua_setglobal(state, "utils");
}
} // namespace solace::script
