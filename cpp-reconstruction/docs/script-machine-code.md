# Lua helper verification against original machine code

`tools/verify_script_machine_code.py` compares the rebuilt numeric/hash helpers
with the original x64 instructions from the identified `Solace.dll`. It does
not use a second C++ or Python implementation of their arithmetic as its oracle.
The original DLL is read as a PE file and copied into Unicorn emulator memory;
Windows never loads it, and no game process is accessed.

The required DLL SHA-256 is
`8e60e1071acd82b08dfd2bbc41b1155e10cfd113075f0ff40ed6372c2f5f232b`.
Another input fails before simulation. The preferred image base is `0x180000000`.
All PE sections are mapped, including the read-only floating-point constant used
by `RemapValClamped`, along with a separate emulator stack and input allocation.

| Helper | Original entry | End, exclusive |
|---|---|---|
| `math.Lerp` | `0x18028AE20` | `0x18028AE95` |
| `math.Clamp` | `0x18028B030` | `0x18028B0BC` |
| `math.RemapVal` | `0x18028B0C0` | `0x18028B199` |
| `math.RemapValClamped` | `0x18028B1A0` | `0x18028B29C` |
| `utils.Fnv1a` | `0x180290320` | `0x180290417` |

The first four boundaries are also recorded in the adjacent `*-asm.json`
evidence. Each function's actual DLL bytes are hashed into the verification
report. A code hook rejects execution outside the selected function, its Lua
boundary calls, and the synthetic return sentinel. The test verifies argument
index order, a single pushed result, normal return, and `RAX == 1`.

## Boundary hooks

Only four Lua API calls are substituted; all arithmetic, comparisons, branches,
loop iterations and hash instructions execute from the original DLL bytes.

| API | Address | Contract supplied/observed |
|---|---|---|
| `luaL_checknumber` | `0x180380230` | EDX is the one-based argument index; XMM0 receives the corresponding binary64 number. |
| `lua_pushnumber` | `0x18037D110` | XMM1 contains the returned binary64 number; its binary32 result bits are compared. |
| `luaL_checklstring` | `0x180380110` | RAX receives the byte buffer; the length is written through the pointer in R8. Embedded NUL bytes are preserved. |
| `lua_pushinteger` | `0x18037D130` | RDX contains the exact integer result. |

MXCSR is reset to `0x1F80` for every case: round-to-nearest, exception masks set,
without flush-to-zero or denormals-are-zero. Input binary32 patterns are widened
to binary64 at the Lua boundary; this includes signaling-NaN quieting during
the conversion. Comparisons retain sign, payload and signed zero: two NaNs with
different bits are reported separately. Without an independent native probe,
any bit difference makes verification fail. With a native probe, each raw NaN
difference must independently match the rebuilt result on the CPU using the
original instruction operand order; no NaN normalization is used.

## Reproduction

The existing source update kit environment contains Unicorn and pefile:

```powershell
../source-update-kit/.venv/Scripts/python.exe tools/verify_script_machine_code.py --dll ../Solace.dll --tool build/Release/solace-tool.exe --native-probe build/Release/native_float_probe.exe --output evidence/script-machine-code.json
```

The normal CMake build also produces `native_float_probe.exe`. Replace these
paths with the executables produced by the chosen build tree.
`solace-tool numeric-check` accepts one JSON object per stdin line, and returns
one JSON object per stdout line:

```json
{"op":"Lerp","bits":[0,1065353216,1056964608]}
{"op":"Fnv1a","hex":"610062"}
```

The respective result shapes are `{"bits":1056964608}` and
`{"value":7337095075662816946}`. The verifier invokes only the newly built local
tool executable, not the original DLL.

The deterministic corpus uses seed `0x501ACE` and 5,706 unique cases. It covers
positive/negative zero, infinities, quiet/signaling NaNs with varied payloads,
subnormals, minimum normal and maximum finite values, one-ULP boundaries,
reversed/equal input and output ranges, extrapolation, randomized float bit
patterns, finite rounding, empty/embedded-NUL/UTF-8 byte strings, unsigned bytes,
and FNV loop/remainder boundaries. The JSON report retains every input and both
outputs, per-function machine-code hashes, the rebuilt tool hash, dependency
versions, coverage counts and mismatch classification.

The verification is bounded to these five helper contracts. It does not verify
the other Lua APIs, callback lifecycle, native module loading, the game host,
or every possible floating-point environment.

## Results and emulator calibration

The original 5,054-case corpus exposed one `Lerp` NaN payload/sign difference.
That finding prompted a larger cross-product of distinct NaN payloads and signs,
and corresponding remap cases. The final corpus contains:

| Helper | Cases | Exact Unicorn matches | CPU-confirmed emulator NaN differences |
|---|---:|---:|---:|
| `Lerp` | 1,471 | 1,446 | 25 |
| `Clamp` | 1,471 | 1,471 | 0 |
| `RemapVal` | 1,235 | 1,231 | 4 |
| `RemapValClamped` | 1,235 | 1,231 | 4 |
| `Fnv1a` | 294 | 294 | 0 |
| Total | 5,706 | 5,673 | 33 |

All 33 raw differences are solely NaN payload/sign differences. An independent
native CPU probe reproduced the rebuilt result for each of those 33 inputs
using the original instruction order. No unresolved value or signed-zero
difference was observed in this corpus.

The first distinguishing case is:

| Input binary32 bits `(first, last, amount)` | Unicorn original instructions | Native original-order probe | Rebuilt Lua helper |
|---|---|---|---|
| `(0x7F800000, 0x7F800000, 0x7FC12345)` | `0x7FC12345` | `0xFFC00000` | `0xFFC00000` |

Disassembly of the actual Release `script.obj` confirmed that `lerp` and its
inlined Lua wrapper preserve `SUBSS(last, first)`, `MULSS(difference, amount)`,
then `ADDSS(scaled, first)`. The compiler did not swap operands. The independent
`tests/native_float_probe.cpp` is compiled with `/Od /fp:strict`; its generated
assembly was also inspected to confirm the same subtraction, multiplication
and addition order. It does not link `solace_core`, Lua, or the original DLL.
The direct CPU multiplication `MULSS(0xFFC00000, 0x7FC12345)` returned
`0xFFC00000`; reversing those operands returned `0x7FC12345`. This identifies
an emulator NaN propagation difference, not a reason to change the C++ helper
to diverge from the original instruction order.

Build the optional calibration probe from a Visual Studio x64 tools shell:

```powershell
New-Item -ItemType Directory build/native-float-probe -Force
cl /nologo /std:c++20 /Od /fp:strict /W4 /WX /EHsc /FAs /Fa"build/native-float-probe/native_float_probe.asm" /Fo"build/native-float-probe/native_float_probe.obj" tests/native_float_probe.cpp /Fe"build/native-float-probe/native_float_probe.exe"
../source-update-kit/.venv/Scripts/python.exe tools/verify_script_machine_code.py --dll ../Solace.dll --tool build/Release/solace-tool.exe --native-probe build/native-float-probe/native_float_probe.exe --output evidence/script-machine-code.json
```

The report retains `exact_match: false` and both raw outputs for all 33 cases,
plus `native_original_order` and `native_matches_rebuilt` for the CPU check.
It records `mismatches: 33`, `confirmed_emulator_nan_differences: 33` and
`unresolved_mismatches: 0`. The verifier exits zero only when every difference
is independently resolved; without `--native-probe`, the raw comparison exits
nonzero. The executable and verifier hashes identify the exact checked build.
This calibration establishes the result on the present x64 CPU and floating-point
environment; it is not a universal NaN payload guarantee across architectures.
