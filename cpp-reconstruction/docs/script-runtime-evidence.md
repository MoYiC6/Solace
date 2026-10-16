# Lua execution, delayed tasks and script lifecycle

Target SHA-256: `8e60e1071acd82b08dfd2bbc41b1155e10cfd113075f0ff40ed6372c2f5f232b`.
Evidence in `evidence/lua-runtime/` is static, read-only, direct IDA JSON-RPC
output. It is not original-DLL runtime execution and is not REA Evidence output.
The target binary is neither linked nor loaded by this implementation.

## Recovered contracts

| Original VA | Evidence and C++ behavior |
|---|---|
| `0x18028A4F0` | Global `print` converts each argument with `luaL_tolstring`, joins with spaces, uses C-string lengths (embedded NUL truncates) and returns no values. C++ records the resulting lines for the host to consume. |
| `0x18028A700` | Global `error` matches Lua's base-library error implementation; the official Lua implementation is reused. `unpack` aliases `table.unpack`. |
| `0x18028A780` | `Delay(number, function)` narrows the number to float32, changes nonfinite delays to zero, clamps negative delay to zero, adds a float clock and retains a Lua registry reference. It returns no values. |
| `0x180272930` | At a present pump, due tasks are extracted in insertion order before invoking them; each reference is released before invocation. Tasks added while running the batch wait for a later pump. Present callbacks follow the delayed tasks for that script. The original also drains input and performs settings/host maintenance outside this recovered subset. |
| `0x180271D50`, `0x18026FE50` | Every 100,000 Lua instructions the watchdog compares `GetTickCount64` with the outer call's 2,000 ms deadline. Nested protected calls inherit the deadline; errors propagate and a Session removes the faulted owner. |
| `0x18027D6C0` | `__lua_script_id` in the Lua registry associates callbacks with a script owner. |
| `0x180270570` | Independent Lua states, monotonically assigned owner IDs, repeated-load handling, source/matching-version bytecode loading with `@filename`, and a `?.lua` module path rooted at the script directory. |
| `0x18026F3B0` | Explicit unload invokes optional `__shutdown` before releasing resources. Faulted scripts are removed without another user shutdown call. GUI settings serialization and native-host teardown remain separate missing work. |

`Services` owns captured log lines and delayed registry references. `Runtime`
owns a Lua state, Services and six CallbackQueues; it closes both owners before
`lua_close`. An error while executing a due task releases all remaining task
references and propagates the actual Lua error. There is no retry or success
substitution. Shutdown releases resources even when `__shutdown` raises.

## Explicit C++ host choices

`Session(root)` is a new standalone integration surface, not the original game
host. Callers supply an existing root directory, choose load/reload/unload,
advance an absolute finite float time with `tick`, and dispatch supported events.
The Session has one Lua state and module cache per loaded file; its operations
must be serialized on the owning thread. A failed script is removed while other
scripts continue. Reload first completes unload; shutdown errors prevent the new
load rather than being hidden. Destructor cleanup does not invoke `__shutdown`;
call `close()` or `unload()` to request it explicitly. Official Lua can still
invoke user `__gc` finalizers during garbage collection and state destruction;
its own `lgc.c` disables debug hooks while finalizers execute. A nonterminating
finalizer therefore cannot be interrupted by this instruction watchdog.

The recovered timer queues schedule against a game clock and pump against a
manager clock. Session intentionally supplies one explicit float clock for both;
real clock integration remains a game-host task. A due time that overflows to
positive infinity stays pending for finite host times, matching the comparison.

Path identity uses canonical filesystem paths, and duplicate files are not
executed twice. Entry paths must remain inside the supplied root; roots containing
Lua path-template delimiters `;` or `?` are rejected. These are standalone host
checks, not a claim that the original had those restrictions. Lua still exposes
`debug`, `loadfile` and other original standard-library capabilities. This is not
a security sandbox or a guarantee against scripts deliberately changing hooks,
registry entries, metatables or package paths. The watchdog interrupts Lua
instruction loops; it does not preempt blocking native calls.

The recovered default deadline is 2,000 ms. Runtime's configurable timeout is a
host/testing extension for source/file execution, delayed tasks, callbacks and
shutdown; all use the recovered 2,000 ms default. Native SEH
handling, original log UI, settings.json/control capture, file watching, ffi,
native command/event userdata, game-event subscription and draw rendering are
not implemented by Session. Unsupported native event dispatch raises an error.

## Verification

`script_services_tests` covers print values and errors, original return counts,
float narrowing/NaN/infinity/negative delay normalization, insertion order,
nested scheduling deferred to the next batch, reference and stack cleanup on
errors, main-thread and coroutine instruction loops, continued use after an
explicit Runtime error, and exactly-once shutdown including failed shutdown.

`script_session_tests` covers separate states, official module loading,
duplicate file loads, due-before-present ordering, event argument contracts,
rejection of native-game arguments without a host, reload, owner failure
isolation, failed compilation, continued cleanup after shutdown failure and
official matching-version Lua bytecode. These are local behavior tests of the
recovered implementation, not a full original loader equivalence measurement.
