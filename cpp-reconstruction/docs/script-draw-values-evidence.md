# Lua draw values: recovered implementation and evidence

The C++ implementation restores the pure value portion of `draw`, using the
existing Lua 5.4.7 dependency and the C++ standard library (`round`, `fmod`). It
does not restore the graphics host or establish full application equivalence.

Source image SHA-256:
`8e60e1071acd82b08dfd2bbc41b1155e10cfd113075f0ff40ed6372c2f5f232b`.
All addresses below are preferred virtual addresses at image base `0x180000000`.
The corresponding RVA is the address minus that base.

## Registration and shared representation

Call `solace::script::register_math_extensions(L)` before
`solace::script::register_draw_values(L)`. Both preserve the Lua stack height.
The latter installs the recovered metatables and pure `draw` names. A preexisting
`draw` table is retained so a later, separately implemented host can add its own
interfaces. This composition behavior is deliberate; the original registration
creates a fresh table containing its host interfaces as well.

| Evidence | Restored contract |
| --- | --- |
| `0x180284280` | `draw.Vec2`, `draw.vec2`, `draw.Rect`, `draw.rect`, `draw.Color`, `draw.color` factories |
| `0x18027B070` | Common protected userdata metatables and their method registrations |
| `0x18027D950` | Draw Vec2 constructor has the same argument/default rules as math's constructor; both draw aliases reuse `math.Vec2` |
| `0x18027DA70` | `lua_rect`: 16 bytes, four floats at offsets 0/4/8/12: x/y/w/h |
| `0x18027DBA0`, `0x18027D880` | `lua_color`: four bytes at offsets 0/1/2/3: R/G/B/A |
| `0x18027D3F0`, common registration | `lua_bits`: one 64-bit unsigned storage word |

The C++ header exposes `Rect`, `Color`, `check_rect`, `check_color`, `check_bits`,
`push_rect`, `push_color`, and `push_bits`. Vectors use `script_math.hpp`'s
`Vec2`/`lua_vec2` representation and checked helper. There is no new invented
Lua bits global or constructor: the evidence only establishes its common
metatable and `None` method. A host can push an initial bits value through the
C++ helper.

## Recovered behavior

Rect method entries at `0x18054E710` are `Clone`, `Contains`, `Offset`, `Expand`,
and `Shrink` (`0x18027CB20` through `0x18027CD10`). The rectangle stores position
and size, not two corners. `Contains` includes both edges and does not normalize
negative sizes. All five operations leave the original rectangle intact.
`Shrink` advances position even when the resulting width or height clamps to
zero. Its SSE maximum also maps a NaN size result to positive zero.

`__index`/`__newindex` at `0x18027CDA0`/`0x18027CE80` expose writable x/y/w/h.
The constructor defaults nonnumeric arguments to zero; field assignment uses
Lua's `tonumber` conversion, so nonnumeric assignments become zero. Unknown
fields remain ignored, exactly as in the original dispatch. This is evidenced
behavior rather than error recovery added by the reconstruction.

Color method entries at `0x18054E770` are `R/G/B/A`, `GetR/GetG/GetB/GetA`,
`Darken`, `Lighten`, `ModA`, and `Interpolate`. The four `Get*` names are aliases:
with no second argument or nil they return a channel integer; with a numeric
second argument they produce a new color with that channel replaced. They do
not mutate the original. Colors support value equality and the original
`color(r, g, b, a)` string representation, with a protected metatable and no
writable channel fields.

Color construction and replacement round halfway away from zero, then clamp to
0..255. Missing or nonnumeric constructor channels default to 255, including
alpha. `ModA`, `Percent`, `Interpolate`, and HSV output use truncation instead of
rounding. Explicit handling of NaN before a C++ integer conversion matches the
original x64 conversion's resulting channel byte without invoking C++ undefined
behavior. The original double thresholds around 0 and 1 in Darken/Lighten and
Interpolate are retained. Arithmetic is compiled with `/fp:strict`.

The color factories are `White`, `WhiteTransparent`, `Black`,
`BlackTransparent`, `Gray`, `Percent`, `RGBA`, `ARGB`, `BGRA`, `ABGR`, `HSV`, and
`Interpolate`. `BGRA` is the same callback as `RGBA`; `ABGR` is the same callback
as `ARGB`. Their names do not cause a channel swap. HSV defaults to 0/0/0/1,
uses hue in degrees modulo 360, wraps negative hue, and clamps saturation,
value, and alpha to 0..1. Helpers `0x18027DDE0` and `0x180338570` establish these
rules; `0x180338360` establishes the per-channel interpolation and packing.

Bits method entries at `0x18054E840` are `None`, `Get`, `Set`, `Unset`, `Toggle`,
`GetRaw`, `SetRaw`, and `Reset`. Indices are 1..64. `Get` and `Set` reject an
out-of-range index, while `Unset` and `Toggle` leave the word unchanged for an
out-of-range integer, as in the original. `Set` uses Lua truthiness; missing
argument clears and numeric zero sets. `GetRaw`/`SetRaw` preserve all 64 bits
through Lua's signed integer representation. `tostring` displays the unsigned
word using `bits(%llu)`.

## Verification

`evidence/lua-draw-values/` contains direct read-only IDA JSON-RPC results,
disassembly where decompilation dropped floating-point operands, and raw PE
registration table/constant extraction. Query records retain the target
identity before and after querying. `validation.json` records final identity,
source and executable hashes, compiler flags, exit code, and actual test output.

`generate_machine_vectors.py` maps the image into Unicorn memory. It executes
the original color/rectangle instructions and their original pure arithmetic
helpers, including the finite-input CRT rounding and fmod code. Only Lua
argument, allocation, metatable attachment, and result boundaries are modeled.
It neither loads the DLL into Windows nor accesses another process. Unexpected
control transfers fail, and each run has an instruction budget.

The checked-in [draw-machine-vectors.json](../tests/fixtures/draw-machine-vectors.json) contains **4,202** original-machine-code
results. `script_draw_values_tests` runs the corresponding expressions through
the real rebuilt Lua runtime and compares all color/rectangle bytes and scalar
results. It also performs **76** behavior checks for registration aliases,
immutability, mutation, numeric strings, missing and invalid arguments, bounds,
bits, and absent host interfaces. The standalone MSVC C++20 build with
`/W4 /WX /fp:strict /MD` passes all **4,278** checks.

The original results were generated from the investigation workspace using the
local analysis environment and the command below. That generator and raw
analysis records are not part of the public repository. The checked-in numeric
fixture is sufficient to build and run the public tests:

```powershell
source-update-kit/.venv/Scripts/python.exe cpp-reconstruction/evidence/lua-draw-values/generate_machine_vectors.py --dll Solace.dll
```

The fixture covers randomized finite colors, rectangles, and HSV, half rounding,
negative/oversized channels, signed zero, NaN/infinite color transformation
amounts, repeated/wrapped HSV hue, inclusive bounds, and negative dimensions.
It is not exhaustive. HSV hue is finite in this fixture; exceptional CRT errno
handling and signaling-NaN payloads are not asserted as equivalent.

## Explicit limits and deliberate memory checks

Original draw registration also creates `surface`, layers, fonts, textures,
time/frame-time, scale, and display interfaces. These require the missing host
and are absent here. The layer method table includes multicolor/gradient draw
operations, but this registration does not expose an independent pure gradient
value or enum table. No such interface is invented or advertised as restored.

The rebuilt userdata accessors check allocation sizes in addition to metatable
identity. A script using `debug.setmetatable` to put a rect/color/bits metatable
on a differently sized userdata therefore receives an explicit error when a
method needs its storage. This is a deliberate memory boundary improvement over
the original unchecked access and is tested. The public C++ push helpers require
the relevant registration to have completed. No host functionality is supplied
as a success-returning placeholder.
