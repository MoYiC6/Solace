#include <solace/script.hpp>
#include <lua.hpp>
#include <nlohmann/json.hpp>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct Scratch {
    std::filesystem::path root;
    Scratch() {
        root = std::filesystem::temp_directory_path() /
            (L"solace-utils-tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        if (!std::filesystem::create_directory(root)) throw std::runtime_error("Temporary root already exists");
    }
    ~Scratch() {
        std::error_code ignored;
        for (const auto& relative : {L"nested/deep/data.bin", L"nested/deep/renamed.bin", L"nested/deep/测试.bin",
                                     L"storage.json", L"nested/deep", L"nested"})
            std::filesystem::remove(root / relative, ignored);
        std::filesystem::remove(root, ignored);
    }
};
std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Could not read temporary database");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    if (!output) throw std::runtime_error("Could not prepare temporary database");
}
}

int main() {
    try {
        Scratch scratch;
        solace::script::Runtime runtime(scratch.root);
        runtime.run(R"lua(
checks = 0
function check(value, message) checks = checks + 1; assert(value, message) end
local names = {'Base64Encode','Base64Decode','GetUnixTime','GetDate','Murmur2','Fnv1a',
  'ClipboardGet','ClipboardSet','JsonEncode','JsonDecode','FileExists','FileRead','FileWrite',
  'FileRemove','FileRename','FileEnumerate','FileCreateDirectories','DbSave','DbLoad'}
local n = 0; for _ in pairs(utils) do n = n + 1 end
check(n == 19, 'utils registration count')
for _, name in ipairs(names) do check(type(utils[name]) == 'function', name) end
local vectors = {{'', ''}, {'f','Zg=='}, {'fo','Zm8='}, {'foo','Zm9v'}, {'foobar','Zm9vYmFy'}}
for _,v in ipairs(vectors) do
  check(utils.Base64Encode(v[1]) == v[2], 'base64 RFC4648 encode')
  check(utils.Base64Decode(v[2]) == v[1], 'base64 RFC4648 decode')
end
local binary = {}; for i=0,255 do binary[#binary+1] = string.char(i) end
binary = table.concat(binary)
check(utils.Base64Decode(utils.Base64Encode(binary)) == binary, 'base64 binary round-trip')
check(utils.Base64Decode(' Z\tm\n9\rv ') == 'foo', 'base64 exact whitespace')
check(utils.Base64Decode('Zm9v!YmFy') == 'foo', 'base64 stops at invalid character')
check(utils.Base64Decode('Zm9v=YmFy') == 'foo', 'base64 stops at padding')
check(utils.Base64Decode('Z') == '', 'base64 incomplete sextet')
check(utils.Base64Decode('Zg') == 'f', 'base64 missing padding')
check(utils.Base64Decode('Zm8') == 'fo', 'base64 missing one padding')
check(utils.Base64Decode('Zm9vZ') == 'foo', 'base64 trailing sextet')
check(utils.Base64Decode('Zm\v9v') == 'f', 'vertical tab is not ignored')
check(utils.Murmur2('') == 0, 'murmur empty')
check(utils.Murmur2('hello') == 0x1e68d17c457bf117, 'murmur hello')
check(utils.Murmur2('12345678') == 0x758f67d162b2d202, 'murmur complete block and sign mask')
local bytes17 = {}; for i=0,16 do bytes17[#bytes17+1] = string.char(i) end
check(utils.Murmur2(table.concat(bytes17)) == 0x20002376a3303d6d, 'murmur blocks and tail')
check(utils.Murmur2('a\0b') == 0x724435ae2d6f1cac, 'murmur embedded NUL')
check(utils.Fnv1a('') == 0x4bf29ce484222325, 'fnv sign mask')
check(utils.Fnv1a('hello') == 0x2430d84680aabd0b, 'fnv known vector')
local before = os.time(); local timestamp = utils.GetUnixTime(); local after = os.time()
check(math.type(timestamp) == 'integer' and timestamp >= before and timestamp <= after, 'Unix timestamp')
before = os.time(); local date = utils.GetDate(); after = os.time()
check(date == os.date('%Y-%m-%d %H:%M:%S', before) or date == os.date('%Y-%m-%d %H:%M:%S', after), 'local date')

check(utils.JsonEncode({}) == '{}', 'empty Lua table is JSON object')
check(utils.JsonEncode({1,true,'a'}) == '[1,true,"a"]', 'dense array')
check(utils.JsonEncode({a=1, [3]='three'}) == '{"3":"three","a":1}', 'object numeric key conversion')
check(utils.JsonEncode({[false]=9,a=1}) == '{"a":1}', 'unrepresentable object key omitted')
check(utils.JsonEncode({a=function() end}) == '{"a":null}', 'unrepresentable value becomes null')
check(utils.JsonEncode(nil) == 'null', 'nil encoding')
check(utils.JsonEncode(0/0) == 'null', 'nonfinite encoding follows JSON dump')
check(utils.JsonEncode({a=1}, true) == '{\n  "a": 1\n}', 'two-space pretty encoding')
check(utils.JsonEncode('a\0b') == '"a\\u0000b"', 'string NUL encoding')
local root = {}; local leaf = root
for i=1,65 do leaf[1]={}; leaf=leaf[1] end
check(utils.JsonEncode(root) == string.rep('[',65)..'null'..string.rep(']',65), 'depth 65 table becomes null')
local decoded = utils.JsonDecode('{"n":4,"x":1.5,"b":true,"s":"a\\u0000b","null":null}')
check(decoded.n == 4 and math.type(decoded.n) == 'integer' and decoded.x == 1.5 and decoded.b and decoded.s == 'a\0b' and decoded.null == nil, 'JSON types')
check(utils.JsonDecode('18446744073709551615') == -1, 'uint64 bit pattern to Lua integer')
check(utils.JsonDecode('{"a\\u0000b":1}').a == 1, 'decoded object key uses C string')
local bad, message = utils.JsonDecode('{')
check(bad == nil and message == 'invalid json' and select('#',utils.JsonDecode('{')) == 2, 'decode failure returns pair')
check(select('#',utils.JsonDecode('null')) == 1, 'JSON null is one nil return')

check(utils.FileCreateDirectories('nested/deep'), 'create directories')
check(utils.FileCreateDirectories('nested/deep'), 'existing directory succeeds')
check(utils.FileExists('nested'), 'directory exists')
check(not utils.FileExists('missing'), 'missing path')
check(utils.FileRead('missing') == nil and select('#',utils.FileRead('missing')) == 1, 'missing read returns nil')
check(not utils.FileWrite('missing-parent/data','text'), 'write failure')
check(utils.FileWrite('nested/deep/data.bin', binary), 'write binary')
check(utils.FileRead('nested/deep/data.bin') == binary, 'read binary')
check(utils.FileWrite('nested/deep/测试.bin','UTF8'), 'Unicode path')
check(utils.FileRead('nested/deep/测试.bin') == 'UTF8', 'Unicode read')
check(utils.FileWrite('nested/deep/renamed.bin','old'), 'prepare rename destination')
check(utils.FileRename('nested/deep/data.bin','nested/deep/renamed.bin'), 'rename replaces destination')
check(not utils.FileExists('nested/deep/data.bin') and utils.FileRead('nested/deep/renamed.bin') == binary, 'renamed content')
local entries=utils.FileEnumerate('nested/deep'); table.sort(entries)
check(#entries==2 and entries[1]=='renamed.bin' and entries[2]=='测试.bin', 'enumerate basenames')
local root_entries=utils.FileEnumerate()
check(#root_entries == 1 and root_entries[1] == 'nested', 'default enumeration and directories')
check(utils.FileRemove('missing'), 'remove missing returns error-code success')
check(utils.FileRemove('nested/deep/测试.bin'), 'remove file')
check(not utils.FileRemove('nested'), 'remove nonempty directory fails')
check(not pcall(utils.FileExists,'../outside'), 'reject parent escape')
check(not pcall(utils.FileExists,'C:/Windows'), 'reject colon')
check(not pcall(utils.FileExists,'bad\npath'), 'reject control byte')
check(utils.FileExists('nested/deep/../deep'), 'canonical in-root path')
check(utils.FileExists('nested\0ignored'), 'original resolver C-string truncation')

check(select('#',utils.DbLoad('missing')) == 0, 'missing database/key returns no values')
check(utils.DbSave('one',{s='hello',n=1}), 'create database')
check(utils.DbSave('two',{1,false,'third'}), 'append database key')
check(utils.DbLoad('one').s=='hello' and utils.DbLoad('one').n==1, 'database prior key retained')
check(utils.DbLoad('two')[3]=='third', 'database array')
check(utils.DbSave('none',nil) and select('#',utils.DbLoad('none'))==1 and utils.DbLoad('none')==nil, 'database stored null')
check(select('#',utils.DbLoad('absent')) == 0, 'database absent key')
check(utils.DbSave('nul\0ignored',42) and utils.DbLoad('nul')==42, 'database key uses C-string semantics')
)lua");
        const auto storage = scratch.root / L"storage.json";
        const auto database = nlohmann::json::parse(read(storage));
        if (database.size() != 4 || database.at("one").at("n") != 1 || !database.at("none").is_null())
            throw std::runtime_error("Database file layout differs from recovered schema");
        for (const auto& invalid : {std::string("{broken"), std::string("[1,2,3]")}) {
            write(storage, invalid);
            runtime.run(R"lua(
local ok, error = pcall(utils.DbSave,'replacement',{})
check(not ok and error:find('original file preserved',1,true), 'corrupt database save must fail')
check(not pcall(utils.DbLoad,'replacement'), 'corrupt database load must fail')
)lua");
            if (read(storage) != invalid) throw std::runtime_error("Corrupt database was overwritten");
        }
        runtime.run(R"lua(
for _, name in ipairs({'Base64Encode','Base64Decode','Fnv1a','Murmur2','JsonDecode','FileExists','DbLoad'}) do
  check(not pcall(utils[name],{}), 'invalid argument '..name)
end
)lua");
        lua_getglobal(runtime.state(), "checks");
        const auto checks = lua_tointeger(runtime.state(), -1);
        solace::script::Runtime no_storage;
        no_storage.run("assert(not pcall(utils.FileExists,'.')); assert(not pcall(utils.DbSave,'key',1)); assert(utils.JsonEncode({})=='{}')");
        {
            solace::script::Runtime finalized(scratch.root);
            finalized.run(R"lua(
local name, context = debug.getupvalue(utils.FileExists, 1)
assert(name ~= nil and type(context) == 'userdata')
local finalize = getmetatable(context).__gc
assert(pcall(finalize, nil))
assert(pcall(finalize, {}))
assert(pcall(finalize, context))
assert(pcall(finalize, context))
local ok, error = pcall(utils.FileExists, '.')
assert(not ok and error:find('storage context is closed', 1, true))
assert(not pcall(utils.DbSave, 'closed', 1))
assert(utils.JsonEncode({}) == '{}')
)lua");
        } // lua_close invokes __gc again after the two manual calls.
        std::cout << "Passed " << checks << " Lua utils assertions, database layout/preservation checks, and empty-root checks.\n"
                     "Manual context finalization, invalid finalizer arguments, closed-root errors, and lua_close passed.\n"
                     "Clipboard functions were registered but not invoked; all file tests used a temporary directory.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Lua utils test failure: " << error.what() << '\n';
        return 1;
    }
}
