# Lua UTF-8 file boundary on Windows

`Runtime` puts an absolute UTF-8 script pattern into `package.path`. Official Lua
5.4.7's package searcher tests candidate files with `fopen`, and its file loader
uses `fopen` followed by `freopen` when it recognizes bytecode. Windows narrow
CRT filenames normally use the active ANSI/OEM code page. A UTF-8 pattern alone
therefore does not make a Chinese or emoji directory loadable.

The implementation keeps the official loader and searcher, including caching,
module-name expansion, BOM/shebang handling, bytecode detection, and Lua error
reporting. It adapts only the filename/mode encoding boundary. Microsoft
documents the narrow filename interpretation and wide-character counterpart in
[fopen / _wfopen](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/fopen-wfopen?view=msvc-170).

## Implementation and build integration

`src/lua_utf8_io.c` provides C-linkage `solace_lua_fopen_utf8` and
`solace_lua_freopen_utf8`. Each converts the NUL-terminated filename and mode with
Windows `MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, ...)`, then delegates
to `_wfopen` or `_wfreopen`. There is no ACP retry or replacement-character
conversion. The existing CRT continues to determine text/binary stream modes;
file contents are not transcoded by this bridge.

`src/lua_utf8_io.h` includes CRT declarations before defining its optional
redirection macros. CMake compiles the new C file into `solace_lua`. Only these
original Lua source files receive `SOLACE_LUA_UTF8_IO_REDIRECT` and a forced
include of that header:

| Official Lua file | Original call sites |
| --- | --- |
| `lauxlib.c` | `luaL_loadfilex`: opening source and reopening bytecode |
| `loadlib.c` | package searcher's `readable` probe |
| `liolib.c` | file opening in the official I/O library |

The vendored Lua files remain unchanged. The bridge source and other project
sources do not receive the redirection definition. MSVC uses
`/FI<absolute-path-to-lua_utf8_io.h>`; the corresponding GCC/Clang option is
`-include <absolute-path>`. The target remains Windows-specific.

## Error and ownership behavior

Malformed UTF-8 produces a null `FILE*`, `errno = EILSEQ`, and the corresponding
Windows conversion error in `_doserrno`. Allocation failure produces `ENOMEM`.
Native open/reopen failure retains the CRT's `errno` and `_doserrno` across
temporary buffer cleanup, so Lua reports its usual file failure instead of
treating it as a successful empty module.

A normal `freopen` attempt consumes the old stream even if opening the new file
fails. Conversion and allocation failures occur before `_wfreopen` can perform
that close. The bridge explicitly closes the old stream in these cases and
restores the original failure code afterward. This matters because
`luaL_loadfilex` overwrites its stream pointer with the return from `freopen`.
The semantics are supported by the
[Microsoft freopen / _wfreopen documentation](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/freopen-wfreopen?view=msvc-170)
and checked against the installed Windows SDK 10.0.26100.0 UCRT
`stdio/freopen.cpp`.

Early parameter failures are separate: a null reopen filename returns `EBADF`
without closing the stream, matching the installed UCRT's unsupported
`freopen(NULL, ...)` path. A null mode/stream or empty reopen filename returns
`EINVAL` before consuming the stream. Null/empty fopen parameters return
`EINVAL`. These bridge checks return failure directly rather than invoking the
CRT invalid-parameter callback. Valid mode parsing remains delegated to the
CRT. Inputs to the C API are ordinary NUL-terminated strings, as with `fopen`.

## Validation

The C bridge and C++ test compile with MSVC `/W4 /WX /MD`. The independent
`lua_utf8_io_tests` run passed **46** checks, including:

- A real `Session` rooted in a Chinese-and-emoji temporary directory, loading a
  Chinese entry filename and requiring Chinese module names.
- Official source and matching-version bytecode modules, module-cache reuse,
  missing-module errors, and BOM/shebang prefixes on both source and bytecode
  imports. The bytecode cases exercise the actual official `freopen` branch.
- The official `io.open` and `loadfile` in a separate Lua state, including binary
  data with an embedded NUL and ordinary file failure results. `Runtime` still
  applies its existing removal of the `io` global.
- Invalid UTF-8 leading bytes, overlong encoding, surrogate encoding, truncated
  sequences, and invalid UTF-8 modes, all rejected with `EILSEQ`.
- Successful Unicode reopening and failed reopening for malformed encoding or
  a missing file. Windows handle inspection confirms that failed normal reopen
  attempts close the previous stream. A null-path early validation failure
  retains it.

This change concerns module imports and official Lua file APIs. `Runtime`'s
separate buffer-based top-level file loading is unchanged. Native DLL module
loading is also unchanged and remains disabled by the current runtime setup.
No locale or process-wide code-page setting is modified.
