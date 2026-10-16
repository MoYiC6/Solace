# Recovered literal resource module

This module embeds 177 resources as immutable C++20 byte arrays: the 146 text
files listed in `resources/manifest.json` and 31 binary files listed in
`resources/binary/manifest.json`. The 337,296 text bytes are preserved verbatim,
including JavaScript and material template placeholders. No terminating NUL is
added to the exposed spans. The binary files add 2,743,833 bytes, making
3,081,129 embedded bytes in total. The original DLL SHA-256 is
`8e60e1071acd82b08dfd2bbc41b1155e10cfd113075f0ff40ed6372c2f5f232b`.

Each `solace::assets::Entry` retains its stable manifest-relative path, evidence
ID, category (`kind`), SHA-256, source PE section, RVA, file offset and template
flag. `all()` enumerates entries in path order; `find(path)` returns an immutable
entry or `nullptr`. Both the entry and its byte span have static lifetime.
`source_path()` records the original manifest's DLL path as provenance; it is
not opened at runtime. `source_sha256()` and `total_bytes()` expose bundle
provenance and size.

## Generation

Run from the reconstruction project directory:

```powershell
python tools/generate_assets.py --manifest resources/manifest.json --binary-manifest resources/binary/manifest.json --output build/generated/solace/assets_generated.hpp
```

Compile `src/assets.cpp` as C++20 with `include` and `build/generated` include
directories. Only `assets.cpp` includes the generated header. The build must
depend on the generator, manifest, and every manifest-listed source file, and
must stop if generation returns nonzero. The generator uses Python 3.10+ standard
library modules; no package installation is required.

Before writing any output it verifies each file's exact length and SHA-256,
UTF-8 encoding, canonical relative path, containment in the manifest directory,
unique paths (also case-insensitive for Windows), unique evidence IDs, RVA/file
boundaries, template flag, aggregate category counts, total count and byte count.
Missing files and inconsistent metadata fail explicitly. Output is generated
deterministically and replaced atomically outside the evidence directory; no
source asset or manifest is edited. Binary entries use exact length/hash and
range validation without inventing text encoding or NUL boundaries. Both manifests
must name the same source DLL hash. Binary entry lookup paths are prefixed with
`binary/`; paths and evidence IDs must remain unique across the combined bundle.
An earlier output is preserved on failure,
but the failing generator exit must block compilation rather than permit stale
generated data to be used.

## Validation and limits

`tests/assets_tests.cpp` checks enumeration, the nine category totals, six
template flags, all stable lookup identities, known source metadata, and missing
or case-mismatched lookups. Generator validation establishes byte-level fidelity;
C++ lookup tests do not independently establish cryptographic integrity.

Validated with MSVC 19.51 using `/std:c++20 /W4 /WX`: the asset test executable
passes. An independent decoder of the generated numeric C++ arrays reproduced
all 146 original SHA-256 digests. Generator checks also exercised deterministic
output and explicit failures for missing files, wrong size, wrong SHA-256,
duplicate paths, category-count mismatch, and total-byte mismatch; failed
generation preserved the previous output while returning a failing exit code.

This is a resource recovery module, not a reconstructed native resource loader.
It does not initialize Panorama, render SVG, compile HLSL/KV3, expand templates,
execute JavaScript, fetch URLs, or prove host compatibility. It does not include
audio or the separate upstream `.rpak` package.
The original manifest's parser results and limitations remain the source of
evidence for syntax and runtime boundaries.

The integrated bundle additionally contains five TTF fonts and 26 PNG images.
Their original slices, format/checksum checks and negative tests are documented
in `binary-assets.md`. No audio or external `.rpak` is included. The separate
`shaders` module compiles the recovered HLSL, but this resource module alone does
not perform compilation or rendering.
