#!/usr/bin/env python3
"""Extract structurally verified, unmodified embedded assets from pinned Solace.dll.

The source PE is opened only for reading. No process is opened and no asset URL
is requested. fontTools and Pillow provide the semantic font/image parsers.
"""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import io
import json
from pathlib import Path
import re
import struct
import sys
import warnings
import wave
import zlib

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
# A task-local dependency installation, not an automatic installation/fallback.
LOCAL_DEPS = HERE / "binary_asset_deps"
if LOCAL_DEPS.is_dir():
    sys.path.insert(0, str(LOCAL_DEPS))

from fontTools import __version__ as FONTTOOLS_VERSION
from fontTools.ttLib import TTFont, TTLibError
from fontTools.ttLib.sfnt import calcChecksum
from PIL import Image, ImageFont, __version__ as PILLOW_VERSION, features

EXPECTED_SHA256 = "8e60e1071acd82b08dfd2bbc41b1155e10cfd113075f0ff40ed6372c2f5f232b"
MAX_IMAGE_PIXELS = 64_000_000


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def check(condition: bool, reason: str) -> None:
    if not condition:
        raise ValueError(reason)


def find_all(data: bytes, signature: bytes):
    start = 0
    while (start := data.find(signature, start)) != -1:
        yield start
        start += 1


def pe_sections(data: bytes) -> tuple[int, list[dict]]:
    check(data[:2] == b"MZ" and len(data) >= 0x40, "invalid DOS header")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    check(pe + 24 <= len(data) and data[pe:pe + 4] == b"PE\0\0", "invalid PE header")
    machine, count = struct.unpack_from("<HH", data, pe + 4)
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    check(machine == 0x8664 and struct.unpack_from("<H", data, optional)[0] == 0x20B,
          "expected AMD64 PE32+")
    image_base = struct.unpack_from("<Q", data, optional + 24)[0]
    section_table = optional + optional_size
    check(section_table + 40 * count <= len(data), "truncated PE section table")
    result = []
    for i in range(count):
        name, virtual_size, rva, size, offset = struct.unpack_from("<8sIIII", data, section_table + 40 * i)
        check(offset + size <= len(data), "PE raw section exceeds file")
        result.append({"name": name.rstrip(b"\0").decode("ascii"), "rva": rva,
                       "virtual_size": virtual_size, "offset": offset, "size": size})
    return image_base, result


def section_at(sections: list[dict], offset: int, size: int = 1) -> dict:
    matches = [s for s in sections if s["offset"] <= offset and
               offset + size <= s["offset"] + min(s["size"], s["virtual_size"])]
    check(len(matches) == 1, "asset must fit in one initialized PE section")
    return matches[0]


def decode_image(blob: bytes, expected: str) -> dict:
    with warnings.catch_warnings():
        warnings.simplefilter("error", Image.DecompressionBombWarning)
        with Image.open(io.BytesIO(blob)) as image:
            check(image.format == expected, "image parser format mismatch")
            check(0 < image.width * image.height <= MAX_IMAGE_PIXELS, "image dimensions out of bounds")
            image.verify()
        with Image.open(io.BytesIO(blob)) as image:
            image.load()
            result = {"width": image.width, "height": image.height, "mode": image.mode,
                      "frames": getattr(image, "n_frames", 1), "decoder": "Pillow",
                      "decoded_pixel_sha256": digest(image.tobytes())}
            for frame in range(1, result["frames"]):
                image.seek(frame)
                image.load()
            return result


def png(data: bytes, start: int, end: int) -> tuple[bytes, dict, str]:
    cursor = start + 8
    chunks = []
    seen_idat = False
    while cursor + 12 <= end:
        size = struct.unpack_from(">I", data, cursor)[0]
        kind = data[cursor + 4:cursor + 8]
        check(size <= 0x7FFFFFFF and cursor + 12 + size <= end, "PNG chunk exceeds section")
        check(all(65 <= x <= 90 or 97 <= x <= 122 for x in kind), "invalid PNG chunk type")
        stored_crc = struct.unpack_from(">I", data, cursor + 8 + size)[0]
        actual_crc = zlib.crc32(data[cursor + 4:cursor + 8 + size]) & 0xFFFFFFFF
        check(actual_crc == stored_crc, "PNG chunk CRC mismatch")
        if not chunks:
            check(kind == b"IHDR" and size == 13, "PNG must begin with IHDR")
        elif kind == b"IHDR":
            raise ValueError("duplicate PNG IHDR")
        seen_idat |= kind == b"IDAT"
        chunks.append({"type": kind.decode("ascii"), "length": size,
                       "crc32": f"0x{stored_crc:08x}", "relative_offset": cursor - start})
        cursor += 12 + size
        if kind == b"IEND":
            check(size == 0 and seen_idat, "invalid PNG IEND or missing IDAT")
            blob = data[start:cursor]
            metadata = decode_image(blob, "PNG")
            metadata.update({"chunks": chunks, "all_chunk_crc32_valid": True,
                             "boundary": "through zero-length IEND including CRC"})
            return blob, metadata, ".png"
    raise ValueError("PNG missing complete IEND")


def sfnt(data: bytes, start: int, end: int) -> tuple[bytes, dict, str]:
    check(start + 12 <= end, "truncated sfnt header")
    count, search_range, entry_selector, range_shift = struct.unpack_from(">HHHH", data, start + 4)
    check(1 <= count <= 128 and start + 12 + count * 16 <= end, "invalid sfnt table count")
    power = 1 << (count.bit_length() - 1)
    check((search_range, entry_selector, range_shift) == (power * 16, power.bit_length() - 1,
                                                        count * 16 - power * 16),
          "invalid sfnt binary-search header")
    tables = []
    for i in range(count):
        tag, checksum, offset, size = struct.unpack_from(">4sIII", data, start + 12 + i * 16)
        check(all(32 <= x <= 126 for x in tag), "invalid sfnt table tag")
        check(offset >= 12 + count * 16 and offset % 4 == 0 and size > 0,
              "invalid sfnt table extent")
        check(start + offset + size <= end, "sfnt table exceeds section")
        table_bytes = data[start + offset:start + offset + size]
        if tag == b"head":
            check(size >= 54 and table_bytes[12:16] == b"\x5f\x0f\x3c\xf5", "invalid head table")
            table_bytes = table_bytes[:8] + b"\0" * 4 + table_bytes[12:]
        check(calcChecksum(table_bytes) == checksum, "sfnt table checksum mismatch")
        tables.append({"tag": tag.decode("ascii"), "offset": offset, "length": size,
                       "checksum": f"0x{checksum:08x}"})
    tags = [x["tag"] for x in tables]
    check(len(set(tags)) == count and tags == sorted(tags), "duplicate or unsorted sfnt tables")
    check({"head", "hhea", "maxp", "name", "cmap", "hmtx"}.issubset(tags),
          "sfnt required tables missing")
    for previous, current in zip(sorted(tables, key=lambda x: x["offset"]),
                                 sorted(tables, key=lambda x: x["offset"])[1:]):
        check(previous["offset"] + previous["length"] <= current["offset"], "overlapping sfnt tables")
    extent = max(t["offset"] + t["length"] for t in tables)
    aligned_extent = (extent + 3) & ~3
    check(start + aligned_extent <= end and data[start + extent:start + aligned_extent] ==
          bytes(aligned_extent - extent), "sfnt terminal padding missing")
    blob = data[start:start + aligned_extent]
    check(calcChecksum(blob) == 0xB1B0AFBA, "whole-font checksum mismatch")
    with TTFont(io.BytesIO(blob), checkChecksums=2, ignoreDecompileErrors=False, lazy=False) as font:
        font.ensureDecompiled(recurse=True)
        cmap = font.getBestCmap()
        check(cmap is not None and len(cmap) > 0, "font lacks usable Unicode cmap")
        names = {str(i): font["name"].getDebugName(i) for i in (0, 1, 2, 4, 6, 13, 14, 16, 17)}
        metadata = {"names": names, "glyph_count": font["maxp"].numGlyphs,
                    "unicode_cmap_entries": len(cmap), "units_per_em": font["head"].unitsPerEm,
                    "tables": tables, "all_table_checksums_valid": True,
                    "whole_font_checksum": "0xb1b0afba", "all_tables_decompiled": True,
                    "boundary": "maximum declared table extent including zero alignment padding"}
        sample = "Solace 0123"
        check(all(ord(char) in cmap for char in sample), "font lacks validation sample characters")
        raster_font = ImageFont.truetype(io.BytesIO(blob), size=24)
        mask = raster_font.getmask(sample, mode="L")
        check(mask.getbbox() is not None, "font sample rasterization produced no pixels")
        metadata["freetype_render"] = {"sample": sample, "pixel_size": 24,
                                       "mask_size": list(mask.size), "mask_bbox": list(mask.getbbox()),
                                       "mask_sha256": digest(bytes(mask))}
    return blob, metadata, ".otf" if data[start:start + 4] == b"OTTO" else ".ttf"


def jpeg_extent(data: bytes, start: int, end: int) -> int:
    cursor = start + 2
    in_scan = False
    while cursor < end:
        if in_scan:
            cursor = data.find(b"\xff", cursor, end)
            check(cursor >= 0, "JPEG missing end marker")
        check(cursor + 2 <= end and data[cursor] == 0xFF, "invalid JPEG marker")
        while cursor < end and data[cursor] == 0xFF:
            cursor += 1
        check(cursor < end, "truncated JPEG marker")
        marker = data[cursor]
        cursor += 1
        if in_scan and (marker == 0 or 0xD0 <= marker <= 0xD7):
            continue
        if marker == 0xD9:
            return cursor
        check(marker not in (0, 0xD8) and not 0xD0 <= marker <= 0xD7, "unexpected JPEG marker")
        check(cursor + 2 <= end, "truncated JPEG segment")
        size = struct.unpack_from(">H", data, cursor)[0]
        check(size >= 2 and cursor + size <= end, "JPEG segment exceeds section")
        cursor += size
        in_scan = marker == 0xDA
    raise ValueError("JPEG missing EOI")


def bounded_image(data: bytes, start: int, end: int, kind: str) -> tuple[bytes, dict, str]:
    # Both an exact boundary and a successful mature decoder are required;
    # random magic bytes in code/font programs cannot become accepted assets.
    if kind == "JPEG":
        stop = jpeg_extent(data, start, end)
    elif kind == "BMP":
        check(start + 26 <= end and data[start + 6:start + 10] == b"\0" * 4, "invalid BMP header")
        stop = start + struct.unpack_from("<I", data, start + 2)[0]
    elif kind == "ICO":
        count = struct.unpack_from("<H", data, start + 4)[0]
        check(1 <= count <= 256 and start + 6 + count * 16 <= end, "invalid ICO directory")
        entries = [struct.unpack_from("<II", data, start + 6 + i * 16 + 8) for i in range(count)]
        check(all(offset >= 6 + count * 16 and size > 0 for size, offset in entries), "invalid ICO extent")
        stop = start + max(size + offset for size, offset in entries)
    else:
        raise ValueError("format detected; exact boundary extractor not implemented")
    check(start < stop <= end, "image extent exceeds section")
    blob = data[start:stop]
    return blob, decode_image(blob, kind), {"JPEG": ".jpg", "BMP": ".bmp", "ICO": ".ico"}[kind]


def riff(data: bytes, start: int, end: int) -> tuple[bytes, dict, str]:
    check(start + 12 <= end, "truncated RIFF header")
    size = struct.unpack_from("<I", data, start + 4)[0]
    check(size >= 4 and start + size + 8 <= end, "RIFF extent exceeds section")
    blob = data[start:start + size + 8]
    cursor = 12
    chunks = []
    while cursor < len(blob):
        check(cursor + 8 <= len(blob), "truncated RIFF chunk header")
        tag = blob[cursor:cursor + 4]
        length = struct.unpack_from("<I", blob, cursor + 4)[0]
        check(cursor + 8 + length + (length % 2) <= len(blob), "RIFF chunk exceeds extent")
        chunks.append({"type_hex": tag.hex(), "length": length})
        cursor += 8 + length + (length % 2)
    if blob[8:12] == b"WEBP":
        metadata = decode_image(blob, "WEBP")
        metadata["riff_chunks"] = chunks
        return blob, metadata, ".webp"
    if blob[8:12] == b"WAVE":
        with wave.open(io.BytesIO(blob), "rb") as audio:
            check(audio.getcomptype() == "NONE", "compressed WAVE not supported")
            frames = audio.readframes(audio.getnframes())
            check(len(frames) == audio.getnframes() * audio.getnchannels() * audio.getsampwidth(),
                  "WAVE frame data truncated")
            metadata = {"channels": audio.getnchannels(), "sample_width": audio.getsampwidth(),
                        "sample_rate": audio.getframerate(), "frames": audio.getnframes(),
                        "decoded_pcm_sha256": digest(frames), "riff_chunks": chunks}
        return blob, metadata, ".wav"
    raise ValueError("RIFF form has no supported decoder")


SIGNATURES = {
    "sfnt_truetype": b"\x00\x01\x00\x00", "sfnt_opentype": b"OTTO",
    "sfnt_apple_true": b"true", "sfnt_apple_type1": b"typ1", "font_collection": b"ttcf",
    "woff": b"wOFF", "woff2": b"wOF2", "png": b"\x89PNG\r\n\x1a\n",
    "jpeg": b"\xff\xd8\xff", "gif87": b"GIF87a", "gif89": b"GIF89a", "dds": b"DDS ",
    "riff": b"RIFF", "bmp": b"BM", "ico": b"\x00\x00\x01\x00", "ogg": b"OggS", "flac": b"fLaC",
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=ROOT.parent / "Solace.dll")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "resources" / "binary")
    parser.add_argument("--manifest", type=Path, default=ROOT / "evidence" / "binary-assets.json")
    parser.add_argument("--asset-manifest", type=Path,
                        help="Build-facing manifest; defaults to OUTPUT_DIR/manifest.json")
    args = parser.parse_args()
    source = args.input.resolve(strict=True)
    output = args.output_dir.resolve()
    manifest = args.manifest.resolve()
    asset_manifest = (args.asset_manifest or output / "manifest.json").resolve()
    check(manifest != source and asset_manifest != source and output != source,
          "output must differ from input")
    check(asset_manifest != manifest, "build and evidence manifests must differ")
    data = source.read_bytes()
    check(digest(data) == EXPECTED_SHA256, "input SHA-256 does not match reviewed original DLL")
    image_base, sections = pe_sections(data)
    records = []
    accepted_offsets = set()
    search = []
    failures = []
    for kind, signature in SIGNATURES.items():
        offsets = list(find_all(data, signature))
        reasons = Counter()
        accepted = 0
        for start in offsets:
            try:
                section = section_at(sections, start)
                end = section["offset"] + min(section["size"], section["virtual_size"])
                if kind.startswith("sfnt_"):
                    blob, metadata, extension = sfnt(data, start, end)
                elif kind == "png":
                    blob, metadata, extension = png(data, start, end)
                elif kind in ("jpeg", "bmp", "ico"):
                    blob, metadata, extension = bounded_image(data, start, end, kind.upper())
                elif kind == "riff":
                    blob, metadata, extension = riff(data, start, end)
                else:
                    raise ValueError("signature detected; no complete decoder in bounded search")
                section_at(sections, start, len(blob))
            except (ValueError, AssertionError, OSError, struct.error, IndexError, KeyError,
                    TTLibError, Image.DecompressionBombError, Image.DecompressionBombWarning) as error:
                reason = str(error) or type(error).__name__
                # Normalize Pillow's address-bearing diagnostics for deterministic evidence.
                reason = re.sub(r"<_io.BytesIO object at 0x[0-9A-Fa-f]+>", "<BytesIO>", reason)
                reasons[reason] += 1
                if kind not in ("sfnt_truetype", "ico"):
                    failures.append({"kind": kind, "file_offset": f"0x{start:08x}", "reason": reason})
                continue
            # Output failures are fatal, not candidate rejection or a false
            # success with fewer assets in the manifest.
            check(start not in accepted_offsets, "duplicate accepted asset offset")
            accepted_offsets.add(start)
            accepted += 1
            label = (metadata.get("names") or {}).get("6") or kind
            label = re.sub(r"[^A-Za-z0-9_.-]+", "_", label).strip("._")
            name = f"{label}_{start:08X}{extension}"
            asset_path = output / name
            check(asset_path.resolve() != source, "asset output aliases source")
            output.mkdir(parents=True, exist_ok=True)
            if asset_path.exists():
                check(asset_path.read_bytes() == blob, "existing asset differs; choose a new output directory")
            else:
                asset_path.write_bytes(blob)
            check(asset_path.read_bytes() == data[start:start + len(blob)], "asset readback mismatch")
            rva = section["rva"] + start - section["offset"]
            records.append({"file": name, "format": extension[1:].upper(), "bytes": len(blob),
                            "sha256": digest(blob), "file_offset": f"0x{start:08x}",
                            "file_end_exclusive": f"0x{start + len(blob):08x}",
                            "rva": f"0x{rva:08x}", "va": f"0x{image_base + rva:x}",
                            "section": section["name"], "byte_exact_readback": True,
                            "validation": metadata})
        search.append({"kind": kind, "signature_hex": signature.hex(), "raw_candidates": len(offsets),
                       "accepted": accepted, "rejected": len(offsets) - accepted,
                       "rejection_reasons": dict(sorted(reasons.items()))})
    records.sort(key=lambda r: int(r["file_offset"], 16))
    check(digest(source.read_bytes()) == EXPECTED_SHA256, "source changed while extracting")
    result = {"source": source.name, "source_sha256": EXPECTED_SHA256, "source_bytes": len(data),
              "image_base": f"0x{image_base:x}", "script_sha256": digest(Path(__file__).read_bytes()),
              "asset_directory": str(Path("..") / "resources" / "binary").replace("\\", "/")
                  if output == ROOT / "resources" / "binary" else str(output),
              "dependencies": {"python": sys.version.split()[0], "fontTools": FONTTOOLS_VERSION,
                               "Pillow": PILLOW_VERSION, "FreeType": features.version("freetype2")},
              "total_assets": len(records), "total_bytes": sum(r["bytes"] for r in records),
              "formats": dict(sorted(Counter(r["format"] for r in records).items())),
              "search": search, "rejected_candidates_except_common_false_signatures": failures,
              "assets": records,
              "limits": ["Bounded raw-signature search of this pinned PE file; no decrypting or unpacking.",
                         "Magic bytes alone are never accepted as an asset.",
                         "No URL from the DLL is accessed and no private resource package is downloaded.",
                         "Names come from font name tables; image filenames state offsets, not inferred UI roles.",
                         "Successful parsing does not identify every runtime resource use or prove complete asset recovery.",
                         "GIF, DDS, TTC, WOFF, WOFF2, OGG and FLAC are signature-counted only; none are accepted without decoders."]}
    manifest.parent.mkdir(parents=True, exist_ok=True)
    manifest.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    build_records = []
    for record in records:
        build_records.append({"path": record["file"],
                              "evidence_id": f"binary_{record['format'].lower()}_{int(record['file_offset'], 16):08X}",
                              "kind": record["format"].lower(), "section": record["section"],
                              "template": False, "encoding": "binary", "length": record["bytes"],
                              "file_offset": record["file_offset"], "rva": record["rva"],
                              "file_end_exclusive": record["file_end_exclusive"],
                              "sha256": record["sha256"], "validation": record["validation"]})
    build_manifest = {"target": source.name, "target_sha256": EXPECTED_SHA256,
                      "total_assets": len(build_records), "total_bytes": result["total_bytes"],
                      "formats": result["formats"],
                      "counts": dict(sorted(Counter(record["kind"] for record in build_records).items())),
                      "script_sha256": result["script_sha256"],
                      "dependencies": result["dependencies"], "assets": build_records,
                      "limits": result["limits"]}
    asset_manifest.parent.mkdir(parents=True, exist_ok=True)
    asset_manifest.write_text(json.dumps(build_manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"assets": result["total_assets"], "bytes": result["total_bytes"],
                      "formats": result["formats"], "manifest": str(manifest),
                      "asset_manifest": str(asset_manifest)}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
