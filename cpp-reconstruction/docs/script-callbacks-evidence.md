# Lua callback queues: recovered behavior and host boundary

This module implements the six queue registration/removal contracts and their common dispatch algorithm in C++20, using the project's existing Lua 5.4.7 implementation for function identity, registry references, garbage collection and protected calls. It does not subscribe to game events or implement the original command/event userdata. Runtime script loading, `__shutdown`, deferred removal, persistence and Delay scheduling belong to the separate Runtime/Session implementation.

Target: `Solace.dll`, SHA-256 `8e60e1071acd82b08dfd2bbc41b1155e10cfd113075f0ff40ed6372c2f5f232b`, image base `0x180000000`. The captures in [lua-callbacks](../evidence/lua-callbacks/) were obtained through the read-only `analysis/ida_query.py` bridge. Each capture includes the checked input identity, request and timestamp. [manifest.json](../evidence/lua-callbacks/manifest.json) hashes the captures and identifies reused evidence. Static decompiler output is evidence of the database's interpretation, not execution of the original binary; its input hash does not certify an unmodified IDA database.

The [registration capture](../evidence/lua-callbacks/register.json) identifies the registration surface. This recovery follows the queue mutation and dispatch bodies as well. Source: [script_callbacks.hpp](../include/solace/script_callbacks.hpp), [script_callbacks.cpp](../src/script_callbacks.cpp), [tests](../tests/script_callbacks_tests.cpp).

## Evidence map

| Address | Evidence | Established behavior |
|---|---|---|
| `0x180289FF0` | [register](../evidence/lua-callbacks/register.json) | `lua_queue`, `events`, six queue kinds and names; original userdata contains two 32-bit fields: script ID and queue kind |
| `0x18027DED0` | [metatable helper](../evidence/lua-callbacks/register-metatable.json) | Method table appears as both `__index` and `methods` |
| `0x18028A410` | [tostring](../evidence/lua-callbacks/tostring.json) | Text `event queue`; registration protects the metatable with `protected queue` |
| `0x18028A290` | [Add](../evidence/lua-callbacks/Add.json) | Checks userdata and function, resolves current script, rejects an unloaded script, appends a reference, returns zero Lua values |
| `0x180272870` | [append reference](../evidence/lua-callbacks/append-reference.json) | `luaL_ref` followed by vector append, without function deduplication |
| `0x18028A330` | [Remove](../evidence/lua-callbacks/Remove.json) | Function comparison uses registry lookup and `lua_rawequal`; stops at the first matching function; missing/unloaded owner is a zero-return no-op after type checks |
| `0x1802728C0` | [remove reference](../evidence/lua-callbacks/remove-reference.json) | Unreference and erase the matching integer reference |
| `0x180272850` | [queue storage](../evidence/lua-callbacks/queue-storage.json) | Six vectors at script offsets 112, 136, 160, 184, 208, 232 |
| `0x18027D6F0`, `0x180272820` | [context](../evidence/lua-callbacks/script-context.json), [lookup](../evidence/lua-callbacks/find-script.json) | Resolve the current script through registry `__lua_script_id`; Add/Remove do not use the userdata's stored ID to choose another script |
| `0x180272930` | [shared runtime evidence](../evidence/lua-runtime/0x180272930.json) | Input queue dispatch, Delay processing, then present queue dispatch |
| `0x1802734D0` | [createMove](../evidence/lua-callbacks/dispatch-create-move.json) | Queue 1, one command userdata argument, only with a current command |
| `0x180273730` | [frameStageNotify](../evidence/lua-callbacks/dispatch-frame-stage.json) | Queue 2, one integer stage argument |
| `0x180273990` | [event](../evidence/lua-callbacks/event-dispatch.json) | Queue 3, one event userdata argument |
| `0x180273C00` | [menuToggled](../evidence/lua-callbacks/dispatch-menu-toggled.json) | Queue 5, one Boolean argument |
| `0x18026FE50` | [protected call](../evidence/lua-callbacks/protected-call.json) | Zero expected callback results; a Lua error ends the current owner's dispatch; error conversion uses `lua_tolstring` and C-string length |
| `0x180270480` | [script error](../evidence/lua-callbacks/script-error.json) | Log/report error, clear active byte at script+80, enqueue script for later removal |
| `0x18026F3B0` | [unload](../evidence/lua-callbacks/unload.json) | Conditional `__shutdown` call, deactivate, save/cleanup and destruction; separate from queue dispatch |
| `0x18026D940`, `0x180279970` | [host subscriptions](../evidence/lua-callbacks/host-events.json), [first thunk](../evidence/lua-callbacks/host-event-thunk.json) | 25 named host subscriptions; the inspected `player_hurt` thunk forwards to the event dispatcher |

## Lua contract and mutation semantics

| Kind | Lua field | Original dispatch arguments |
|---|---|---|
| 0 | `events.presentQueue` | None |
| 1 | `events.createMove` | One command userdata |
| 2 | `events.frameStageNotify` | One integer |
| 3 | `events.event` | One event userdata |
| 4 | `events.input` | Three integers from a queued input record |
| 5 | `events.menuToggled` | One Boolean |

Each queue exposes `Add(function)` and `Remove(function)` using method-call syntax. Both return zero values. Add stores duplicate registrations independently. Remove deletes the first registration with the same Lua function identity, leaving later duplicates. Passing an equivalent new closure does not match the old function.

Dispatch copies the queue's **integer registry references**, checks that each reference is still in the live queue before calling it, and checks the active/state flags between calls. It discards callback return values. Removing a future registration skips it. Adding a fresh reference defers that registration to the next dispatch. Removing a reference and then adding a function may reuse that reference through Lua's free list: if that integer occurs later in the snapshot, the replacement function runs in the same dispatch. The C++ code deliberately preserves this detail instead of pinning a second set of function objects or assigning new generation IDs.

A callback error stops that script's remaining callbacks and deactivates all of its queues. Other scripts can continue through their own queue managers. Add and Remove still work while the owner is inactive but its Lua state remains present, matching the original methods' state-only check. Once closed, Add raises `the script has already been unloaded`; Remove retains its zero-return no-op, with argument validation still performed. Non-string errors report `unknown error (non-string error value)`; numbers accepted by `lua_tolstring` become text, and embedded NUL truncates the copied original-style error message.

The 25 event names are exposed as read-only metadata by `registered_event_names()`. Their presence does not install listeners, create event objects, or establish the behavior of all 25 native forwarding thunks.

## Explicit C++ host integration and additions

```cpp
// state has the script's libraries and registry __lua_script_id initialized.
solace::script::CallbackQueues queues(state, owner_id, solace::script::protected_call);
lua_pushinteger(state, stage);
auto result = queues.dispatch(solace::script::Queue::frame_stage, 1);
// Session records result.error and arranges unloading when lua_status != LUA_OK.
queues.close();
lua_close(state);
```

The `protected_call` function above is supplied by Runtime. `ProtectedCall` is a `std::function` so Runtime can bind its configured deadline; it must obey the `lua_pcall` stack/status contract for the requested argument and result counts. Native exceptions must not cross Lua C frames. If the host adapter itself throws after returning to C++, dispatch restores its stack boundary and deactivates the owner before propagating the exception. The queue tests use Lua's own `lua_pcall` directly; Runtime's watchdog is covered by separate service tests. The original native-exception handler and host/TLS guards are outside this module.

The host dispatch API is new: callers explicitly choose the queue and push arguments on its bound Lua state. `dispatch(queue, nargs)` copies those arguments for each callback and consumes the original arguments even when the queue is empty or inactive. Values below them remain on the stack. Table/userdata identity is shared across callbacks by this API; it does not reconstruct the native command/event wrapper constructors that the original dispatcher called for each callback. The host is responsible for supplying actual values and valid lifetimes. There is no automatic dispatch, engine pointer, memory access, fabricated event or successful default Host response.

The original queue userdata was eight bytes with no observed `__gc` registration. The recovered C++ owner has a longer lifetime relationship, so it adds a registry anchor and shared control block. Queue and anchor GC handlers verify metatable, size and marker, and destroy the shared handle only once. Repeated/manual GC, close-before-Lua-close and Lua-close-before-C++-destruction are covered. GC of an individual queue userdata releases its handle but does not remove registered callbacks; closing the owner unreferences all callbacks. These are explicit C++ ownership provisions, not claims about original binary memory layout. The registry owner ID is checked against the bound owner; mismatches produce the unloaded behavior instead of looking up another state's storage. Coroutine registration uses the shared Lua registry.

All operations are serialized on the Lua state's owner thread. A host may deactivate or close the queue manager while a callback runs; it must not destroy/close the Lua state until the dispatch returns. Invalid enum/argument counts, an owner-ID mismatch during construction, duplicate installation, and dispatch after close are explicit C++ errors. `deactivate()` is one-way; a new script state is required to restart an unloaded script.

## Validation

The initial independent module passed **36 scenarios**, built locally with MSVC C++20, `/MD /W4 /permissive- /utf-8`, against Lua 5.4.7 without warnings. Its historical source/binary hashes are preserved in [validation.json](../evidence/lua-callbacks/validation.json). Integration adds a 37th scenario for a throwing host adapter, plus the Runtime callback deadline test. Final integrated source/binary hashes and actual CTest output are recorded in `BUILD_PROOF.json`; the earlier standalone record is not a hash claim for the subsequently edited source.

The scenarios cover all six queues, names/metatables, zero returns and argument validation, duplicate identity, first removal, order, mutations during dispatch including reference reuse and vector reallocation, explicit arguments and stack preservation, discarded results, per-owner error isolation, original error conversion, inactive/closed behavior, reference release, owner mismatch, wrong-size userdata, repeated GC, both Lua/C++ shutdown orders, natural queue collection, coroutine registration and invalid host calls.

These tests establish local C++/Lua behavior against the stated static evidence. They do not establish original-binary equivalence under a running game, complete Lua API compatibility, native exception recovery, all event object methods, or a complete Solace reconstruction.
