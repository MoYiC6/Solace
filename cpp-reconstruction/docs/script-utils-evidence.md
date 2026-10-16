# Lua utils recovery evidence

The C++ module implements the 19 functions in the recovered `utils` table. This
is a standalone Lua utility module, not a reconstruction of game/engine Lua
bindings or of the complete Solace runtime. It never loads or executes the
original DLL.

The evidence target is `Solace.dll`, image base `0x180000000`, SHA-256
`8e60e1071acd82b08dfd2bbc41b1155e10cfd113075f0ff40ed6372c2f5f232b`.
The 36 JSON captures in `cpp-reconstruction/evidence/lua-utils/` contain the
IDA target identity, method, arguments, capture time, and returned decompilation
or assembly. They were read using `analysis/ida_query.py` against the local IDA
RPC endpoint. The manifest in that directory hashes each capture. Addresses
below are original virtual addresses, not addresses in the rebuilt program.

Public C++ entry point:

```cpp
void solace::script::register_utils(
    lua_State* state, const std::filesystem::path& storage_root);
```

It installs global `utils` and copies `storage_root` into Lua-owned userdata.
Registration does not create directories or files. An empty root raises
`lua folder unavailable` when a file/database function is called; the other
functions remain usable. Each Lua state owns its own root. The caller must
provide an existing usable directory or create it explicitly.

The userdata owns a path pointer and has an idempotent finalizer. Because the
runtime exposes Lua's debug library, scripts can obtain a closure upvalue and
invoke its `__gc` manually. The finalizer checks userdata type/size, releases the
path once, and nulls its pointer; a later automatic finalization is harmless.
File/database access after manual finalization raises `storage context is
closed`. Invalid finalizer arguments are ignored. The metatable is attached
before allocating the path, so ownership is already finalizable on later errors.

Dependencies are the project's Lua 5.4.7, nlohmann/json, C++20 standard library,
and Windows `Crypt32`/`User32`. Base64 uses Crypt32 after the recovered input
normalization; JSON parsing/serialization and filesystem operations use the
mature libraries rather than custom parsers. `Fnv1a` reuses the project's
`solace::script::fnv1a`. `Murmur2` reconstructs the small MurmurHash64A algorithm
identified by the original constants and assembly; this is Austin Appleby's
public-domain MurmurHash2 variant, with the original Solace sign-bit mask.

| Lua function | Original address | Implemented behavior |
|---|---|---|
| `Base64Encode(bytes)` | `0x18028FBF0` | Standard alphabet, `=` padding, no line breaks; binary strings retained |
| `Base64Decode(text)` | `0x18028FF00` | Skip only HT/LF/CR/space; stop at first other nonalphabet byte, including `=`; emit complete decoded bytes and discard trailing incomplete bits |
| `GetUnixTime()` | `0x1802900C0` | Integer Unix seconds from precise Windows FILETIME |
| `GetDate()` | `0x180290110` | Local time string `YYYY-MM-DD HH:MM:SS` |
| `Murmur2(bytes)` | `0x1802901F0` | MurmurHash64A, seed zero, result masked by `0x7FFFFFFFFFFFFFFF` |
| `Fnv1a(bytes)` | `0x180290320` | FNV-1a 64-bit with the same sign-bit mask |
| `ClipboardGet()` | `0x180290420` | Read `CF_UNICODETEXT`, return UTF-8; failures explicitly raise an error |
| `ClipboardSet(text)` | `0x180290590` | UTF-8 to Unicode clipboard, return boolean success |
| `JsonEncode(value, pretty)` | `0x1802906B0` | Recursive conversion below; Lua-truthy `pretty` selects two-space indentation |
| `JsonDecode(text)` | `0x1802907A0` | Recursive conversion below; syntax error returns exactly `nil, "invalid json"` |
| `FileExists(path)` | `0x180290870` | Boolean existence of file or directory; filesystem status errors return false |
| `FileRead(path)` | `0x180290920` | Binary string; open failure returns one nil; subsequent read failure raises an error |
| `FileWrite(path, bytes)` | `0x180290AB0` | Binary truncating write; boolean success; no implicit parent directory creation |
| `FileRemove(path)` | `0x180290BE0` | Nonrecursive removal; boolean based on error code, so already missing returns true |
| `FileRename(old, new)` | `0x180290C80` | `MoveFileExW` with replace-existing and copy-allowed flags; boolean success |
| `FileEnumerate(path)` | `0x180290D90` | Missing/nil path means `.`; nonrecursive unsorted array of UTF-8 basenames, including directories |
| `FileCreateDirectories(path)` | `0x180291070` | Recursive directory creation; error-code success, including an existing directory |
| `DbSave(key, value)` | `0x180291120` | Merge value into `storage.json` object and compactly serialize; boolean write success |
| `DbLoad(key)` | `0x1802916B0` | Missing database/key returns zero Lua values; stored JSON null returns one nil |

Each API's primary evidence is the correspondingly named JSON file. Supporting
functions are preserved separately:

| Evidence file | Purpose |
|---|---|
| `0x180291A30.json`, `0x180291A30-asm.json` | Lua to JSON conversion, table classification, supported key/value types, depth comparison |
| `0x1802922D0.json` | JSON to Lua conversion, integer handling, array indexing, C-string object keys |
| `0x180296480.json` | Relative path conversion, rejected bytes, canonicalization and component comparison |
| `0x1802720E0.json` | Weak canonicalization helper |
| `0x1802969A0.json` | Original Lua folder plus `storage.json` |
| `0x1802918E0.json` | UTF-16 to UTF-8 conversion |
| `0x1803B8230.json` | Precise FILETIME and Unix epoch adjustment |
| `0x18042EBC4.json`, `0x18042D61C.json`, `0x18042D624.json` | `_time64`, local-time conversion, timezone/DST behavior |
| `0x1803BC8C0.json`, `0x180290870-asm.json` | File status helper and its call arguments |
| `0x1803BCEF0.json` | Filesystem remove error handling |
| `0x1803BD220.json` | `MoveFileExW(..., 3)` |
| `0x180063310.json` | Filename extraction for enumeration |
| `0x1802901F0-asm.json` | MurmurHash64A constants, block/tail operations and sign mask |

JSON conversion details are intentionally retained, including behaviors that
do not form a bijective Lua/JSON mapping:

- Booleans, signed Lua integers, floating-point values and length-bearing
  strings map directly. Nil and unsupported Lua value types map to JSON null.
  nlohmann/json serializes nonfinite floating-point values as null.
- A table is an array only when its nonzero `lua_rawlen` equals its total key
  count. Values are then read with raw integer indices `1..length`. Otherwise
  it is an object; an empty table therefore encodes as `{}`. This preserves the
  original raw-length heuristic rather than adding a different array detector.
- Object keys are only strings or numbers converted by Lua to strings. Other
  key types are omitted. Object values are recursively converted before that
  key check, as in the original helper. Embedded NUL bytes are retained during
  encoding. Table conversion at recursion depth 65 or greater becomes null;
  the assembly compares depth with `0x41` before processing that table.
- JSON arrays become Lua indices starting at 1. JSON null becomes nil, so an
  array null creates a hole and an object null removes the corresponding key.
  Unsigned JSON integers use their signed 64-bit bit pattern. Decoded object
  keys pass through `lua_setfield`, which truncates a key at an embedded NUL.
  The decoder has no extra invented recursion cutoff; Lua stack growth errors
  are reported.
- Invalid JSON returns two values. Valid JSON null returns one value. These
  return counts are distinct from the database's zero-value missing result.

Path behavior comes from `0x180296480`. The original application supplies
`%APPDATA%/solace_int/lua`; this module requires the caller-supplied root instead
and never discovers or accesses APPDATA itself. Path arguments use the original
C-string interpretation, so an embedded NUL terminates the argument. Before
that terminator, `:` and byte values below `0x20` are rejected. UTF-8 conversion
uses Windows code page `CP_UTF8` with flags zero. The root and candidate undergo
`std::filesystem::weakly_canonical`, then root components are compared with
`_wcsicmp`. A normalized in-root `..` is accepted; leaving the root raises an
error. The root directory itself is a valid result. Canonicalization errors
raise an error. As in the original path-based design, the resolve-then-open
sequence is not a guarantee against concurrent filesystem changes.

Database layout is one JSON object at `<storage_root>/storage.json`. Keys use
C-string interpretation. `DbSave` converts the Lua value through the same JSON
conversion and dump/parse normalization as the original `JsonEncode` call,
retains other database members, then writes compact JSON. A missing database
starts a new object. No storage directory is created implicitly. Writes retain
the original truncating file semantics; atomic transactions, backups and
cross-process locking have not been invented.

There are deliberate error-handling differences required by the user's
instruction that failures must not be concealed:

- Original `DbSave` initializes an object and only replaces it when an opened
  database parses as an object (`DbSave.json`, original lines 76–91,
  `0x18029120E–0x180291295`). A malformed/nonobject existing file or a failed
  read can therefore be silently discarded before the later truncating write.
  The reconstruction instead raises an error for an unreadable, malformed or
  nonobject existing database and preserves its bytes. `DbLoad` also raises
  for those states; original lines 41–51 otherwise leave the return count zero.
  A genuinely absent database remains the normal creation/missing case.
- Original `ClipboardGet` continues with an empty string after open, data or
  lock failure (`ClipboardGet.json`, original lines 25–57). These failures and
  conversion/close failures now raise explicit errors. `ClipboardSet` still
  returns a success boolean and also checks clipboard empty/close outcomes.
- Original `ClipboardSet` opens with a null HWND, then empties the clipboard.
  Microsoft documents that this sets a null owner and causes `SetClipboardData`
  to fail. The reconstruction deliberately fixes that defect: it creates a
  temporary message-only window using the Windows `STATIC` class, prepares the
  Unicode allocation before opening/emptying the clipboard, and opens with that
  valid HWND. It closes the clipboard after every successful open, frees memory
  unless ownership transferred to Windows, and destroys the temporary window
  before returning. Creation, unlock, close, free and destruction failures are
  reflected in the boolean result. This is a behavior repair, not byte-equivalent
  recovery. See Microsoft's [OpenClipboard contract](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-openclipboard)
  and [message-only windows](https://learn.microsoft.com/en-us/windows/win32/winmsg/window-features#message-only-windows).
- File/database writes explicitly close their streams before testing success,
  so both buffered write and close errors reach the caller. File reads explicitly
  report a bad stream after opening. Status/open failures still use the recovered
  false/nil contracts listed above.

C++ exceptions are caught by the registered wrapper, their messages are placed
on the Lua stack, and `lua_error` is called after the implementation and catch
scopes have exited. Argument checks occur before owning C++ local objects.
This avoids explicit `luaL_error`/`lua_error` long jumps across live C++ RAII
objects on ordinary argument, filesystem and JSON error paths. As with the Lua
C API generally, allocator exhaustion inside a Lua allocation is not converted
into a C++ exception.

Verification used a standalone MSVC C++20 build with `/W4 /permissive- /fp:strict`
and the actual Lua 5.4.7 static library. The test program passed 105 Lua
assertions plus C++ database schema, byte-preservation and empty-root checks.
An additional isolated Runtime verifies invalid finalizer arguments, two manual
`__gc` calls, explicit errors on subsequent file/database use, and destruction
through `lua_close` without another deletion.
The cases cover the 19 registrations, Base64 vectors and invalid-prefix rules,
binary strings, hash vectors, clocks, JSON recursion/types/return counts,
Unicode paths, directory and replacement-rename behavior, path rejection,
database merge and stored null, malformed/nonobject database preservation, and
argument errors. All file tests use a newly created temporary directory.
Clipboard functions were registered but not invoked; interactive clipboard
behavior has not been exercised. The temporary message-only `STATIC` window
creation/destruction was verified separately without opening the clipboard.
The original DLL was neither executed nor
used as a runtime test oracle, so this is evidence-based reconstruction with
the explicit differences above, not a claim of exhaustive binary equivalence.
