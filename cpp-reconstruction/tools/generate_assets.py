#!/usr/bin/env python3
"""Verify recovered assets and emit their immutable C++20 representation.

This reads only manifest-listed local files. It never runs asset text, rewrites
templates, accesses their URLs, or reads/modifies the original DLL.
"""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import sys
import tempfile


class AssetError(ValueError):
    pass


def unique_object(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise AssetError(f"duplicate JSON key: {key!r}")
        result[key] = value
    return result


def integer(value: object, label: str, maximum: int = (1 << 64) - 1) -> int:
    if type(value) is not int or not 0 <= value <= maximum:
        raise AssetError(f"{label}: expected nonnegative integer <= {maximum}")
    return value


def address(value: object, label: str, maximum: int = (1 << 64) - 1) -> int:
    if not isinstance(value, str) or not re.fullmatch(r"0x[0-9A-Fa-f]+", value):
        raise AssetError(f"{label}: expected hexadecimal address")
    return integer(int(value, 16), label, maximum)


def string(value: object, label: str) -> str:
    if not isinstance(value, str) or not value or "\0" in value:
        raise AssetError(f"{label}: expected nonempty NUL-free string")
    return value


def digest(value: object, label: str) -> str:
    value = string(value, label)
    if not re.fullmatch(r"[0-9a-fA-F]{64}", value):
        raise AssetError(f"{label}: expected SHA-256 hexadecimal digest")
    return value.lower()


def cpp_string(value: str) -> str:
    # Explicit UTF-8 bytes make the result independent of compiler source encoding.
    return '"' + "".join(f"\\x{byte:02x}" for byte in value.encode("utf-8")) + '"'


def verify(manifest_path: Path, binary: bool = False) -> tuple[dict, list[dict]]:
    manifest_path = manifest_path.resolve(strict=True)
    root = manifest_path.parent
    manifest = json.loads(
        manifest_path.read_text(encoding="utf-8-sig"), object_pairs_hook=unique_object
    )
    if not isinstance(manifest, dict):
        raise AssetError("manifest: expected object")
    string(manifest.get("target"), "target")
    manifest["target_sha256"] = digest(manifest.get("target_sha256"), "target_sha256")
    records = manifest.get("assets")
    if not isinstance(records, list) or not records:
        raise AssetError("assets: expected nonempty array")
    expected_count = integer(manifest.get("total_assets"), "total_assets")
    expected_bytes = integer(manifest.get("total_bytes"), "total_bytes")
    if len(records) != expected_count:
        raise AssetError(f"total_assets: expected {expected_count}, found {len(records)}")

    paths: set[str] = set()
    evidence_ids: set[str] = set()
    verified = []
    for index, item in enumerate(records):
        label = f"assets[{index}]"
        if not isinstance(item, dict):
            raise AssetError(f"{label}: expected object")
        path = string(item.get("path"), f"{label}.path")
        relative = PurePosixPath(path)
        if (relative.is_absolute() or "\\" in path or ":" in path
                or ".." in relative.parts or path != relative.as_posix()):
            raise AssetError(f"{label}.path: expected canonical relative POSIX path: {path!r}")
        if path.casefold() in paths:
            raise AssetError(f"duplicate asset path: {path}")
        paths.add(path.casefold())
        try:
            asset_path = (root / path).resolve(strict=True)
        except OSError as error:
            raise AssetError(f"{path}: missing or unreadable resource: {error}") from error
        if not asset_path.is_relative_to(root) or not asset_path.is_file():
            raise AssetError(f"asset is not a regular file within manifest directory: {path}")
        evidence_id = string(item.get("evidence_id"), f"{label}.evidence_id")
        if evidence_id in evidence_ids:
            raise AssetError(f"duplicate evidence_id: {evidence_id}")
        evidence_ids.add(evidence_id)
        kind = string(item.get("kind"), f"{label}.kind")
        section = string(item.get("section"), f"{label}.section")
        is_template = item.get("template")
        if type(is_template) is not bool:
            raise AssetError(f"{label}.template: expected boolean")
        encoding = "binary" if binary else "utf-8"
        if item.get("encoding") != encoding:
            raise AssetError(f"{path}: expected declared {encoding} encoding")
        length = integer(item.get("length"), f"{path}.length")
        file_offset = address(item.get("file_offset"), f"{path}.file_offset")
        rva = address(item.get("rva"), f"{path}.rva", (1 << 32) - 1)
        end = address(item.get("file_end_exclusive"), f"{path}.file_end_exclusive")
        if file_offset + length != end:
            raise AssetError(f"{path}: inconsistent original file boundaries")
        if not binary:
            nul = address(item.get("nul_terminator_offset"), f"{path}.nul_terminator_offset")
            if end != nul:
                raise AssetError(f"{path}: inconsistent original text terminator")
        sha256 = digest(item.get("sha256"), f"{path}.sha256")
        payload = asset_path.read_bytes()
        if len(payload) != length:
            raise AssetError(f"{path}: size mismatch: expected {length}, found {len(payload)}")
        actual_digest = hashlib.sha256(payload).hexdigest()
        if actual_digest != sha256:
            raise AssetError(f"{path}: SHA-256 mismatch: expected {sha256}, found {actual_digest}")
        if not binary:
            payload.decode("utf-8", errors="strict")
        verified.append(dict(path=path, evidence_id=evidence_id, kind=kind,
                             section=section, is_template=is_template, length=length,
                             file_offset=file_offset, rva=rva, sha256=sha256,
                             payload=payload))

    actual_bytes = sum(item["length"] for item in verified)
    if actual_bytes != expected_bytes:
        raise AssetError(f"total_bytes: expected {expected_bytes}, found {actual_bytes}")
    counts = manifest.get("counts")
    if not isinstance(counts, dict):
        raise AssetError("counts: expected object")
    for kind, count in counts.items():
        integer(count, f"counts.{kind}")
    actual_counts = dict(Counter(item["kind"] for item in verified))
    if counts != actual_counts:
        raise AssetError(f"counts mismatch: expected {counts}, found {actual_counts}")
    return manifest, sorted(verified, key=lambda item: item["path"])


def render(manifest: dict, records: list[dict]) -> str:
    lines = [
        "// Generated by generate_assets.py; verified against the recovered manifest.",
        "// Do not edit. Literal bytes are data; no asset is executed by this table.",
        "#pragma once", "#include <array>", "#include <solace/assets.hpp>",
        "namespace solace::assets::generated {",
        f"inline constexpr std::string_view source_path = {cpp_string(manifest['target'])};",
        f"inline constexpr std::string_view source_sha256 = {cpp_string(manifest['target_sha256'])};",
        f"inline constexpr std::size_t total_bytes = {manifest['total_bytes']};",
    ]
    for index, item in enumerate(records):
        payload = item["payload"]
        lines.append(f"inline constexpr std::array<std::uint8_t, {len(payload)}> data_{index:04d}{{{{")
        for start in range(0, len(payload), 24):
            lines.append("    " + ", ".join(f"0x{value:02x}" for value in payload[start:start + 24]) + ",")
        lines.append("}};")
    lines.append(f"inline constexpr std::array<Entry, {len(records)}> entries{{{{")
    for index, item in enumerate(records):
        fields = [cpp_string(item[key]) for key in ("path", "evidence_id", "kind", "sha256", "section")]
        fields.extend((f"0x{item['rva']:x}U", f"0x{item['file_offset']:x}ULL",
                       "true" if item["is_template"] else "false", f"data_{index:04d}"))
        lines.append("    Entry{" + ", ".join(fields) + "},")
    lines.extend(("}};", "} // namespace solace::assets::generated", ""))
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--binary-manifest", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        manifest, records = verify(args.manifest)
        if args.binary_manifest:
            binary_manifest, binary_records = verify(args.binary_manifest, binary=True)
            if binary_manifest["target_sha256"] != manifest["target_sha256"]:
                raise AssetError("binary assets belong to a different original DLL")
            for item in binary_records:
                item["path"] = "binary/" + item["path"]
            records.extend(binary_records)
            if len({item["path"].casefold() for item in records}) != len(records):
                raise AssetError("duplicate path across asset manifests")
            if len({item["evidence_id"] for item in records}) != len(records):
                raise AssetError("duplicate evidence ID across asset manifests")
            records.sort(key=lambda item: item["path"])
            manifest["total_assets"] = len(records)
            manifest["total_bytes"] += binary_manifest["total_bytes"]
        result = render(manifest, records).encode("ascii")
        output = args.output.resolve()
        # Never allow generation to overwrite the evidence inputs.
        if output.is_relative_to(args.manifest.resolve().parent) or (args.binary_manifest and output.is_relative_to(args.binary_manifest.resolve().parent)):
            raise AssetError("output must be outside the recovered-assets directory")
        output.parent.mkdir(parents=True, exist_ok=True)
        # Keep existing output intact on validation failure; the nonzero exit
        # stops the build. Atomic replacement also prevents truncated headers.
        with tempfile.NamedTemporaryFile(dir=output.parent, prefix=output.name + ".",
                                         suffix=".tmp", delete=False) as stream:
            temporary = Path(stream.name)
            try:
                stream.write(result)
                stream.flush()
                os.fsync(stream.fileno())
            except BaseException:
                stream.close()
                temporary.unlink(missing_ok=True)
                raise
        try:
            os.replace(temporary, output)
        finally:
            temporary.unlink(missing_ok=True)
        print(f"Verified {len(records)} assets / {manifest['total_bytes']} bytes; wrote {output}")
        return 0
    except (AssetError, OSError, UnicodeError, json.JSONDecodeError) as error:
        print(f"asset generation failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
