# Lua math and vector recovery evidence

This module adds eight registered names from the original `math` table and
restores its two vector userdata types. Together with the four functions already
in `script.cpp`, 12 of the original 13 `math` names are implemented.
`WorldToScreen` is absent because it requires real host projection state.
These functions do not load or execute the original DLL.

Evidence target: `Solace.dll`, image base `0x180000000`, SHA-256
`8e60e1071acd82b08dfd2bbc41b1155e10cfd113075f0ff40ed6372c2f5f232b`.
`cpp-reconstruction/evidence/lua-math/` contains direct read-only IDA RPC
captures made through `analysis/ida_query.py`, with target metadata checked
before and after every request. The raw table/constant record additionally
reads the original PE without executing it, checks its SHA-256 and records RVA
mapping, file offsets and bytes. `manifest.json` hashes the evidence files.
IDA decompilation of floating-point code frequently omits arguments or entire
expressions here, so the implementation uses the captured assembly for those
operations, operand order and branches.

Integration entry point is
`solace::script::register_math_extensions(lua_State*)`. It requires the standard
Lua `math` table and preserves its existing fields. Call it before the draw value
registration. It creates the original metatables only when they do not already
exist. Shared C++ helpers are declared in `solace/script_math.hpp`:

```cpp
struct Vec2 { float x{}, y{}; };       // 8 bytes, lua_vec2
struct Vec3 { float x{}, y{}, z{}; };  // 12 bytes, lua_vector
Vec2 check_vec2(lua_State*, int);
Vec3 check_vec3(lua_State*, int);
void push_vec2(lua_State*, Vec2);
void push_vec3(lua_State*, Vec3);
```

The helpers use full userdata, not Lua tables. `check_*` uses Lua argument errors
and must be called before constructing RAII locals. Constructors allocate exactly
8 or 12 bytes and attach `lua_vec2` or `lua_vector`, as shown by the recovered
helpers `0x18027D7B0` and `0x18027D740`. This module additionally validates userdata
size before reading it, so a wrong-size userdata with a forged metatable reports
an argument error instead of reading beyond its storage. Vector objects contain
only floats and require no C++ finalizer.

| Registered name | Original VA | Recovered behavior |
|---|---|---|
| `CalcAngle(source, destination)` | `0x18028A8B0` | Subtract destination minus source; pitch `atan2f(dz, horizontal) * -57.295780181884766f`, yaw `atan2f(dy, dx) * 57.295780181884766f`, roll zero; one adjustment per ±180 boundary |
| `AngleNormalize(number)` | `0x18028A9D0` | Float32 input; `fmodf(value + 180, 360)`, add 360 only for a negative remainder, then add -180 |
| `ApproachAngles(current, target, speed)` | `0x18028AA50` | Speed defaults to 10, then original MAXSS with 0.001; each component uses `fmodf((target-current)+540,360)-180`, clamps by original MINSS/MAXSS operand order and adds current |
| `EdgePoint(low, high, direction, inset)` | `0x18028AC40` | Positive direction selects high, negative selects low, zero/unordered selects midpoint; positive inset adjusts x/y inward by sign, leaving z unchanged |
| `VectorAngles(direction)` | `0x18028AEA0` | Horizontal magnitude >0.001 uses `atan2f(-z,horizontal)*180/pi` and `atan2f(y,x)*180/pi`, with separate float32 multiply/divide; small/unordered horizontal uses pitch 270 for positive z, otherwise 90; then one boundary adjustment |
| `Vec2(x, y)` | `0x18028B2A0` | Missing/nil y duplicates x; other provided nonnumeric arguments use the original numeric helper's zero default |
| `Vec3(x, y, z)`, `Vector(x, y, z)` | `0x18028B300` | Each argument independently uses the original numeric helper's zero default; both names register the same C function |

The seven named function-body captures and their assembly captures establish
the eight table names, including the `Vector` alias.
The numeric helper `0x18027DA30` calls the Lua-number predicate and converts with
`lua_tonumber`, accepting numeric strings; it returns its supplied default when
the predicate fails. Constructor zero defaults, `ApproachAngles` speed 10 and
`EdgePoint` inset zero are original API behavior, not new recovery fallbacks.
`AngleNormalize` instead uses a checked numeric argument and errors on a
nonnumeric input.

Notable preserved differences and edge cases:

- `CalcAngle(zero, zero)` has negative-zero pitch, zero yaw and zero roll.
  `VectorAngles(zero)` has pitch 90. These are separate original algorithms.
- `CalcAngle` multiplies by the float bits `0x42652EE1`; `VectorAngles` multiplies
  by 180 and then divides by float pi `0x40490FDB`. Combining those operations
  would change rounding and is deliberately avoided.
- `ApproachAngles` uses the original `+540` before remainder, including for
  deltas below -540. It does not first normalize arbitrarily large angles.
  For current 0, target -1000 and speed 1000, the resulting x is -280.
- `MINSS/MAXSS` choose their second operand on equal or unordered inputs.
  Helpers preserve that ordering instead of substituting `std::fmin/fmax`.
- `EdgePoint` with zero direction selects midpoint, then adds a positive inset
  to x and y. The inset does not modify z. NaN direction follows the original
  unordered branch to midpoint.

Type registration is captured at `0x18027B070`. The original method tables at
`0x18054E540` and `0x18054E670` were read directly from the hash-checked PE. All
registered methods below are implemented, with individual decompilation and
assembly captures under their original addresses:

| Type | Methods |
|---|---|
| `lua_vector` | `IsZero`, `Clone`, `Dist`, `DistSqr`, `Dist2d`, `Dist2dSqr`, `Cross`, `IsValid`, `Length`, `LengthSqr`, `Length2d`, `Length2dSqr`, `Dot`, `AngleVectors`, `VectorAngles`, `GetForward`, `Normalize`, `Normalized` |
| `lua_vec2` | `Clone`, `Floor`, `Ceil`, `Round`, `Len`, `LenSqr`, `Dist`, `DistSqr`, `Dot` |

Each type also has its original nine metamethods: `__index`, `__newindex`,
`__add`, `__sub`, `__mul`, `__div`, `__unm`, `__eq` and `__tostring`. Their
metatables are protected by the original strings `protected vector` and
`protected vec2`. There is no invented `__len` or arbitrary field storage.

- Fields are lowercase x/y/z or x/y. Field names use the original C-string
  test, including an embedded NUL directly after a coordinate letter. Unknown
  writes are ignored. Numeric strings on writes are converted; unsupported
  write values become zero through `lua_tonumber`, as in `0x18027C290` and
  `0x18027C940`. Non-field reads look in the metatable's `methods` table.
- Arithmetic is component-wise and supports a vector or a true Lua number on
  either side. Unlike constructors, arithmetic rejects numeric strings.
  Division by a zero component yields positive zero, as shown in
  `0x18027E6F0`/`0x18027E890`; this is an original operation, not an invented
  error substitute. Equality compares the stored floats. String conversion
  uses the original two-decimal formats and 96/64-byte buffers.
- Length/distance/dot calculations preserve float32 multiplication and the
  original `zTerm + (xTerm + yTerm)` addition order. No double-precision
  intermediate or fused multiply-add is introduced. `Cross` preserves
  handedness. `IsValid` requires all three components to be finite.
- `Normalize` mutates only for squared length >0 and returns the prior length;
  its square root is computed even when the comparison is unordered.
  `Normalized` returns a new zero vector for squared length zero or unordered,
  and computes its square root only in the positive branch. Those different
  NaN outcomes come from `0x18027C090` and `0x18027C110`.
- `AngleVectors` returns forward, right, up in that order, using float
  radians-per-degree bits `0x3C8EFA35`. At zero angles the vectors are (1,0,0),
  (0,-1,0), (0,0,1). `GetForward` returns only the first result.
  The vector method `VectorAngles` has the same numeric algorithm as the math
  function, confirmed against its own assembly at `0x18027BF20`.
- Vec2 rounding returns a new vector. `Round` rounds halfway cases away from
  zero; its original helper leaves values with magnitude >=2^23, including
  infinity/NaN bit patterns, unchanged.

The implementation reuses Lua 5.4.7 and mature C++ math operations: float
`std::sqrt`, `std::atan2`, `std::sin`, `std::cos`, `std::fmod`, `std::floor`,
`std::ceil` and `std::round`. The original sin/cos exception helpers explicitly
name `sinf`/`cosf` (`0x1804512D0`/`0x180451380`); the remainder helper names
`fmodf` (`0x180435B50`). The original CRT has CPU-dispatched implementations.
Their entire polynomial/exception machinery is not copied. `/fp:strict` is
required for the reconstructed evaluation order. Exact NaN payloads, floating
exception flags and cross-CRT transcendental bit equivalence are not claimed.

Validation uses a standalone MSVC C++20 `/W4 /permissive- /fp:strict` executable
linked to the project's real Lua 5.4.7 static library. It passes 75 Lua assertions
and 2,550 native checks. The native tests transcribe the captured SSE sequence
for `AngleNormalize` and `ApproachAngles`, independently using SIMD intrinsics
for arithmetic, MINSS/MAXSS and bitwise selection. They compare result bits over
positive/negative zero, boundaries, large angles, infinities, NaNs and 2,527
current/target/speed combinations. NaNs compare by class rather than payload.
Other checks cover vector userdata size, sign of zero, registration retention,
all vector methods/metamethods, constructor defaults, type errors, angle
direction/thresholds and zero-vector behavior. This instruction-reference check
does not execute original DLL code and is not an original-runtime oracle;
transcendental examples use explicit geometric expected values and a small
tolerance. No file, clipboard or game-host operation is needed by the tests.
