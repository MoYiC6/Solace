#include <solace/script.hpp>
#include <solace/script_session.hpp>
#include "../src/lua_utf8_io.h"
#include <lua.hpp>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <io.h>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
unsigned checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
std::string utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
struct Scratch {
    std::filesystem::path root;
    std::vector<std::filesystem::path> files;
    Scratch() : root(std::filesystem::temp_directory_path() /
        (L"solace-lua-中文-😀-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        require(std::filesystem::create_directory(root), "Cannot create unique UTF-8 test root");
    }
    ~Scratch() {
        std::error_code ignored;
        for (const auto& file : files) std::filesystem::remove(root / file, ignored);
        std::filesystem::remove(root, ignored);
    }
    void write(const std::filesystem::path& file, const std::string& data) {
        files.push_back(file);
        std::ofstream output(root / file, std::ios::binary);
        output.write(data.data(), static_cast<std::streamsize>(data.size()));
        output.close();
        require(static_cast<bool>(output), "Cannot write UTF-8 test file");
    }
};
void set_string(lua_State* L, const char* name, const std::string& value) {
    lua_pushlstring(L, value.data(), value.size()); lua_setglobal(L, name);
}
void run(lua_State* L, const char* source) {
    if (luaL_dostring(L, source) != LUA_OK) throw std::runtime_error(lua_tostring(L, -1));
}
HANDLE native_handle(FILE* stream) {
    return reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stream)));
}
void require_closed(HANDLE handle) {
    DWORD flags = 0;
    require(GetHandleInformation(handle, &flags) == FALSE && GetLastError() == ERROR_INVALID_HANDLE,
            "Failed freopen retained the old stream handle");
}
}

int main() {
    try {
        Scratch scratch;
        scratch.write(L"模块.lua", "return {value = 41}");
        scratch.write(L"带标记.lua", "\xEF\xBB\xBF#!/usr/bin/lua\nreturn {value = 42}");
        scratch.write(L"入口.lua", R"lua(
            assert(require('模块').value == 41)
            assert(require('带标记').value == 42)
            assert(require('字节码').value == 43)
            assert(require('带标记字节码').value == 43)
            assert(require('模块') == require('模块'))
            local ok, err = pcall(require, '不存在')
            assert(not ok and err:find('not found', 1, true))
            print('utf8-module-source-bytecode-bom-shebang-ok')
        )lua");
        solace::script::Runtime compiler;
        require(luaL_loadstring(compiler.state(), "return {value = 43}") == LUA_OK, "Bytecode compilation failed");
        std::string bytecode;
        const auto writer = [](lua_State*, const void* data, std::size_t length, void* target) -> int {
            static_cast<std::string*>(target)->append(static_cast<const char*>(data), length); return 0;
        };
        require(lua_dump(compiler.state(), writer, &bytecode, 0) == 0, "Bytecode dump failed");
        lua_pop(compiler.state(), 1);
        scratch.write(L"字节码.lua", bytecode);
        scratch.write(L"带标记字节码.lua", "\xEF\xBB\xBF#!/usr/bin/lua\n" + bytecode);
        solace::script::Session session(scratch.root);
        const auto id = session.load(L"入口.lua");
        require(id != 0 && session.scripts().size() == 1, "Unicode Session module load failed");
        const auto logs = session.take_logs();
        require(logs.size() == 1 && logs[0].text == "utf8-module-source-bytecode-bom-shebang-ok",
                "Unicode source/bytecode import result differs");
        require(session.close().failures.empty(), "Session close failed");

        // Exercise the official io library too. Runtime intentionally hides io;
        // this independent state verifies the same linked Lua C implementation.
        lua_State* L = luaL_newstate();
        require(L != nullptr, "Cannot allocate raw Lua state");
        try {
            luaL_openlibs(L);
            set_string(L, "source_path", utf8(scratch.root / L"模块.lua"));
            set_string(L, "bytecode_path", utf8(scratch.root / L"字节码.lua"));
            set_string(L, "missing_path", utf8(scratch.root / L"不存在.lua"));
            set_string(L, "bad_utf8", std::string("invalid-\xFF.lua"));
            scratch.files.push_back(L"写入.txt");
            set_string(L, "write_path", utf8(scratch.root / L"写入.txt"));
            run(L, R"lua(
                local f = assert(io.open(source_path, 'rb'))
                assert(f:read('*a') == 'return {value = 41}'); assert(f:close())
                f = assert(io.open(write_path, 'wb')); assert(f:write('binary\0bytes')); assert(f:close())
                f = assert(io.open(write_path, 'rb')); assert(f:read('*a') == 'binary\0bytes'); assert(f:close())
                assert(assert(loadfile(source_path))().value == 41)
                assert(assert(loadfile(bytecode_path))().value == 43)
                local value, err, code = io.open(missing_path, 'rb')
                assert(value == nil and type(err) == 'string' and type(code) == 'number')
                value, err, code = io.open(bad_utf8, 'rb')
                assert(value == nil and type(err) == 'string' and type(code) == 'number')
                value, err = loadfile(bad_utf8)
                assert(value == nil and type(err) == 'string')
            )lua");
            ++checks;
            lua_close(L);
        } catch (...) { lua_close(L); throw; }

        const auto source = utf8(scratch.root / L"模块.lua");
        const auto compiled = utf8(scratch.root / L"字节码.lua");
        FILE* file = solace_lua_fopen_utf8(source.c_str(), "rb");
        require(file != nullptr, "Direct UTF-8 open failed");
        require(std::fgetc(file) == 'r', "Direct UTF-8 stream content differs");
        file = solace_lua_freopen_utf8(compiled.c_str(), "rb", file);
        require(file != nullptr, "UTF-8 reopen failed");
        require(std::fgetc(file) == LUA_SIGNATURE[0], "UTF-8 binary reopen altered file contents");
        require(std::fclose(file) == 0, "UTF-8 close failed");
        for (const std::string bad : {std::string("\xFF"), std::string("\xC0\xAF"),
                                      std::string("\xED\xA0\x80"), std::string("\xF0\x9F\x98")}) {
            errno = 0;
            require(solace_lua_fopen_utf8(bad.c_str(), "rb") == nullptr && errno == EILSEQ,
                    "Malformed UTF-8 path did not fail with EILSEQ");
            file = solace_lua_fopen_utf8(source.c_str(), "rb");
            require(file != nullptr, "Cannot prepare old stream");
            const HANDLE handle = native_handle(file);
            errno = 0;
            require(solace_lua_freopen_utf8(bad.c_str(), "rb", file) == nullptr && errno == EILSEQ,
                    "Malformed UTF-8 reopen did not preserve EILSEQ");
            require_closed(handle);
        }
        errno = 0;
        require(solace_lua_fopen_utf8(source.c_str(), "\xFF") == nullptr && errno == EILSEQ,
                "Malformed UTF-8 mode was accepted");
        file = solace_lua_fopen_utf8(source.c_str(), "rb");
        require(file != nullptr, "Cannot prepare failed-mode stream");
        HANDLE old_handle = native_handle(file);
        require(solace_lua_freopen_utf8(source.c_str(), "\xFF", file) == nullptr && errno == EILSEQ,
                "Invalid mode conversion did not fail");
        require_closed(old_handle);

        file = solace_lua_fopen_utf8(source.c_str(), "rb");
        require(file != nullptr, "Cannot prepare missing-path stream");
        old_handle = native_handle(file);
        const auto missing = utf8(scratch.root / L"不存在.lua");
        errno = 0;
        require(solace_lua_freopen_utf8(missing.c_str(), "rb", file) == nullptr && errno == ENOENT,
                "Native failed reopen errno changed");
        require_closed(old_handle);
        errno = 0;
        require(solace_lua_fopen_utf8(nullptr, "rb") == nullptr && errno == EINVAL, "Null fopen path handling differs");
        file = solace_lua_fopen_utf8(source.c_str(), "rb");
        require(file != nullptr, "Cannot prepare parameter-error stream");
        require(solace_lua_freopen_utf8(nullptr, "rb", file) == nullptr && errno == EBADF,
                "Null freopen path handling differs");
        require(std::fgetc(file) == 'r', "Early parameter failure closed the old stream");
        require(std::fclose(file) == 0, "Final stream close failed");
        std::cout << "Lua UTF-8 I/O: " << checks << " checks passed, including real Chinese Session require, bytecode reopening, and failed-stream closure\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
