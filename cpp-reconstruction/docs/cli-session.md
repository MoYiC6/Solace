# Script session JSON-lines protocol

`solace-tool script-session ROOT` creates one explicit script host for the existing directory `ROOT`. It reads one UTF-8 JSON object per stdin line and writes one compact JSON reply per command to stdout, flushing after each reply. Prompts and script print text never appear outside the JSON stream. There is no automatic event or timer source.

The Windows entry point receives UTF-16 arguments through `wmain`; filesystem arguments remain wide paths. The `key` command converts category/name arguments to UTF-8 with strict Windows character validation, matching the recovered byte-based hashing contract.

From the `cpp-reconstruction` directory in PowerShell:

```powershell
Get-Content examples/session-commands.jsonl | .\build\Release\solace-tool.exe script-session examples/scripts
$LASTEXITCODE
```

[session-commands.jsonl](../examples/session-commands.jsonl) exercises [lifecycle.lua](../examples/scripts/lifecycle.lua), including delayed execution, four event kinds, reload and shutdown. The example starts a fresh process, so the first successful load has ID 1 and its reload gets ID 2.

## Commands

All listed fields are required. Unknown or duplicate fields, unknown operations, non-object input, malformed JSON, comments and blank lines produce failed replies. Numeric strings, Boolean values in integer fields and floating-point spellings such as `1.0` in integer fields are rejected.

| Operation | Request | Behavior |
|---|---|---|
| Load | `{"op":"load","file":"lifecycle.lua"}` | Load a nonempty relative UTF-8 path under ROOT; canonical path containment is checked by Session. NUL bytes and rooted paths are rejected. Loading an already loaded canonical file returns its existing ID. |
| Reload | `{"op":"reload","id":1}` | Shut down and remove the old script, then load its file into a fresh state with a new ID. |
| Unload | `{"op":"unload","id":1}` | Remove the script and explicitly invoke its `__shutdown`, if present. |
| Tick | `{"op":"tick","time":0.5}` | Supply absolute seconds to Session. For each script, due Delay callbacks run before its present queue. |
| Dispatch | `{"op":"dispatch","queue":"present","args":[]}` | Invoke only the present queue, without advancing time or executing Delay tasks. |
| Dispatch | `{"op":"dispatch","queue":"frame_stage","args":[5]}` | One signed 64-bit integer. |
| Dispatch | `{"op":"dispatch","queue":"input","args":[65,1,0]}` | Three signed 64-bit integers, passed in order. |
| Dispatch | `{"op":"dispatch","queue":"menu_toggled","args":[true]}` | Exactly one JSON Boolean. |
| List | `{"op":"list"}` | Return currently loaded script IDs, filenames and pending Delay counts. |
| Close | `{"op":"close"}` | Shut down all loaded scripts, report shutdown errors/logs, then end the CLI session. Later input is not read. |

IDs must be JSON integers in `1..4294967295`; unknown loaded IDs are errors. Event integers must be in `-9223372036854775808..9223372036854775807`. No narrowing or wraparound is used to accept out-of-range integers.

Time accepts JSON numbers that convert to a finite float32. Float32 overflow and nonzero values that underflow to zero are rejected; ordinary float32 rounding is expected. Time is an absolute value, not a delta, and the current Session API permits negative values or moving backward. The reply includes the float32 time actually passed to Session. The CLI does not expose native game-event or command queues, since those require unrecovered host userdata.

## Replies and failure handling

Every reply has `op`, `line`, `ok`, `results`, `logs` and `errors`. `line` is one-based for input commands. `results.scripts` is the current script list after the operation, including failed operations that already changed state. Operation-specific results include the loaded `id`, reload's `previous_id`, `unloaded_id`, or `callbacks`/`delayed_tasks` counts from Session.

```json
{"op":"load","line":1,"ok":true,"results":{"id":1,"scripts":[{"id":1,"file":"lifecycle.lua","delayed_tasks":1}]},"logs":[{"id":1,"file":"lifecycle.lua","text":"loaded lifecycle.lua"}],"errors":[]}
```

`logs` contains captured script print messages, associated with their script ID and filename. They are drained once per reply, including messages printed before a command fails. Command errors have `scope:"command"` and `error`; per-script errors from Session reports have `scope:"script"`, `id`, `file` and `error`. UTF-8 text is represented as a JSON string. If a log or error contains invalid UTF-8 bytes, the affected field is `null` and an additional `<field>_bytes` array preserves the exact unsigned byte values. This is explicit binary representation, not replacement characters or lost messages.

A failed command returns `ok:false`; subsequent lines continue to execute. Any request, script, startup or shutdown failure makes the final process exit code **1**, even if later commands succeed. A session with no failures exits **0**. A startup failure emits a single `op:"startup"`, `line:0` failed reply. Output stream failure is reported to stderr with a failing process exit.

If stdin reaches EOF before an explicit close, the CLI explicitly closes Session and emits an additional `op:"close"`, `reason:"eof"` reply. Its line number is the next input-line number. Shutdown logs and failures are included; EOF does not silently bypass `__shutdown`. A stdin read error similarly closes the session and reports `reason:"input_error"` with `ok:false`. An explicit close already terminates the session and does not produce a second EOF reply.

Reload is not transactional: shutdown failure removes the old script, and a failure loading the new state leaves it unloaded. Session also removes scripts that fail during a callback; other loaded scripts continue. The resulting script list makes these outcomes visible. Close gathers shutdown failures from individual scripts while continuing to close the remaining scripts. Pending Delay tasks are released when a script closes.

## `run-lua` output

`solace-tool run-lua FILE` now prints captured load/run logs to stdout, explicitly calls `Runtime::shutdown()`, and prints shutdown logs. It attempts shutdown even after a run error, so a previously defined `__shutdown` function can report its own result. Run and shutdown errors are reported to stderr after the captured logs have been flushed; either error produces exit code 1. It does not advance time or synthesize queue events, so pending Delay callbacks are not executed by this command.

This CLI is an explicit host for the recovered local C++ modules. It does not establish complete original Solace behavior, reconstruct native game objects, or attach to a game. Its Lua libraries and script APIs are those currently implemented by Runtime; it is not a general security sandbox.

## Integration checks

Run the sample JSONL and expect 12 valid replies, exit code 0, delayed messages before present messages at times 0.5 and 1, shutdown logs on reload/unload, and no remaining scripts after close. Also test EOF with a loaded script, load/dispatch/shutdown errors with prior print messages, malformed and duplicate fields followed by a valid list command, signed integer boundaries and overflow, forbidden queues, float32 overflow, and invalid UTF-8 print bytes. `run-lua` should retain prints preceding an error and output shutdown prints with the appropriate exit code.

The persistent regression runner is [verify_cli_session.py](../tools/verify_cli_session.py):

```powershell
python tools/verify_cli_session.py --tool build/Release/solace-tool.exe
```

It uses only Python's standard library and a temporary directory. The current implementation passed **14 scenarios / 324 checks**, including Chinese paths, the existing fixed-byte UTF-8 key vector, all protocol/error cases above and generated configuration files. It does not read original profiles, use the clipboard or interact with a game. This run used a separately linked CLI with the latest `main.cpp` and the currently built core/Lua libraries; the final integrated CMake/CTest run remains a separate check.
