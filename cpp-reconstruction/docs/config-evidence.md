# C++ configuration recovery evidence

This module reconstructs the version 3 **data format and file storage**. It does
not claim that the complete Solace field registry, menu bindings, reset defaults,
legacy migration, or custom serializers have been recovered. It never loads or
calls `Solace.dll`.

Evidence target SHA-256:
`8e60e1071acd82b08dfd2bbc41b1155e10cfd113075f0ff40ed6372c2f5f232b`.
All evidence paths below are relative to the repository root.

| Implemented behavior | Evidence |
|---|---|
| Category/name each truncated to 120 bytes; join with `.`; C-string length; FNV-1a 64-bit; low 32 bits; eight lowercase hexadecimal digits | `analysis/config_trace/decompile_1800271e0.json`, `decompile_180005b80.json`, `decompile_180027f80.json`, `decompile_1802498d0.json` |
| `{"version":3,"fields":{...}}` document | `analysis/config_trace/decompile_1802498d0.json`, `decompile_18024e0b0.json` |
| Field types 0–8: setting, bool, signed int, uint8, float, RGBA bytes, float3, counted bool array, string | `analysis/config_trace/decompile_18024a6c0.json` |
| Setting `{"v":bool,"b":...}`; binding `{"k":int,"m":int,"l":bool}`; zero key saves null | `analysis/config_trace/decompile_18024be30.json` |
| Missing binding `k`/`m` read as 0, missing `l` reads as true | `analysis/config_trace/decompile_18024f270.json`, lines 15–18 and labels 12/13 |
| Root `%APPDATA%/solace_int/configs`, filename `<name>.json`, sorted enumeration | `analysis/config_trace/decompile_180247f60.json`, `decompile_180247a90.json`, `decompile_180247360.json`, `decompile_180249ef0.json` |
| Open existing, shared read, reject zero bytes or >64 MiB, require full read | `analysis/config_trace/decompile_180247c90.json` |
| JSON indent 2 spaces; `CreateFileW(GENERIC_WRITE,share=0,CREATE_ALWAYS)`; full write check | `analysis/config_trace/decompile_1802478e0.json` |

`Binding.mode` and `Binding.l` deliberately preserve their wire representation.
No UI semantics or mode enumeration is assigned. A zero-key binding becomes
`std::nullopt`; the raw document retains the original JSON unless that particular
field is explicitly re-encoded.

The embedded-NUL hash behavior is grounded in the original joiner, rather than
an assumption about a formatted string: `CT-decompile_1800271e0` lines 86–90
copy category bytes with an explicit count, write byte 46 at `dst[category_len]`,
copy name bytes to `dst + category_len + 1`, and append a final NUL. Line 91 calls
`sub_180005B80`, whose line 3 obtains the C-string length before the FNV loop.
A NUL copied from the category consequently precedes the appended dot and ends
the hashed string. The joiner does not use `sprintf("%.*s.%.*s", ...)`.

An additional read-only disassembly of the original PE bytes confirms the two
library helpers: file offset `0x45C210` / RVA `0x45CE10` dispatches by the byte
count in `r8` and copies source memory without testing for NUL. File offset
`0x45CE50` / RVA `0x45DA50` scans until a zero byte: its scalar path at
`0x18045DA60` loads `[rax]`, increments `rax`, tests that byte for zero and jumps
to `0x18045DAC8`, whose `lea rax,[rax+rcx-1]; ret` returns the length using the
negated original pointer. The aligned path also tests individual bytes for zero.
Only bytes were disassembled; no target code was executed.

## Public API

- `make_key`, `key_string`: recovered key derivation.
- `encode_value`, `decode_value`, `FieldValue`, `FieldType`: explicit conversion
  for supported data types. Custom type 9 throws an explanatory `Error`.
- `ConfigDocument::parse/from_json/load_file`: validate the document envelope,
  retain unknown fields and top-level metadata.
- `ConfigDocument::get/set/dump/save_file`: typed access and version 3 storage.
- `Registry::bind/apply/capture`: bind **only caller-supplied** C++ values. Apply
  returns separate applied, missing, and unbound key lists. Missing keys leave
  the current caller value unchanged; no recovered default is inferred.
- `default_directory`, `create_directory`, `profile_path`, `list_profiles`:
  explicit path operations. Reading `default_directory()` creates nothing.

`ConfigDocument` round-trip preservation means equality of the parsed JSON data,
including unknown members. Formatting, member ordering, and numeric lexemes are
not preserved byte-for-byte. A typed float read/write intentionally converts to
the recovered 32-bit float storage type. Merely opening and saving a document
does not perform that conversion.

## Explicit reconstruction policies

The original loader's permissive/error behavior is not fully recovered. This
library rejects unsupported versions, malformed types, out-of-range integers,
non-finite floats, duplicate JSON members, noncanonical field keys and mismatched
array sizes. Bound values are validated before applying them; that validation
order is this library's policy, not a claim about original loader atomicity.
In particular, the original loader at `0x18024E0B0` calls `0x18024E890` using
`version >= 3` before applying fields (evidence lines 69–71). Its reset/default
phase is not implemented by `Registry::apply`. Therefore this module is a
document editor and explicit binding API, **not the complete original loader**;
leaving an absent caller binding unchanged must not be described as Solace's
missing-field reset behavior.
Missing binding `k`, `m`, `l` defaults above are supported because the original
binding decoder explicitly proves them. Application defaults remain unknown.

Only version 3 is accepted. The original has a `version >= 3` branch, but that
does not establish future-version compatibility. Type 9 is retained as raw JSON
when unbound and rejected for typed decoding. Its serializers must be recovered
individually. Path traversal characters are rejected for profile names; raw
`load_file`/`save_file` accept an explicit caller-selected path. Writes use the
recovered direct overwrite semantics, without a temporary replacement file.

`upstream/velocity/.../external/config.hpp` is **not format-compatible by itself**:
it uses version 2, LZ4-compressed registry values below
`HKCU/Software/velokitty/configs`, and its binding serializer omits `l`. Its
matching type layout and key algorithm support reuse of the data model, not
blind reuse of its persistence backend. JSON parsing/serialization here uses
the project's vendored official `nlohmann/json.hpp`.

## Validation

`config_tests` checks recovered keys `playerlist.priority -> 40a5e9e6` and
`peek assistance.quick peek color -> 3428e79d`, byte truncation, embedded NUL,
all nine supported value kinds, binding defaults, malformed data rejection,
unknown-member preservation, explicit registry reports, whole-document
validation before application, file I/O and the 64 MiB file boundary.

Optional argument `config_tests <original-config-directory>` reads every `.json`
profile in that directory, saves copies under a newly created temporary test
directory, reloads the copies, checks JSON equality, and verifies the original
bytes are unchanged. It does not write to APPDATA. Test cleanup removes only the
files the test itself created and its own temporary directory.

Validation performed on 2026-10-16 with MSVC, C++20, `/W4 /permissive- /fp:strict`:
the standalone build completed without compiler warnings. The test executable
passed **62 checks**, including semantic round-trips of all **5 existing version
3 profiles**, each containing **2,266 fields**. Each source profile's bytes were
compared before and after the test and remained unchanged. These data tests do
not establish menu or runtime compatibility.
