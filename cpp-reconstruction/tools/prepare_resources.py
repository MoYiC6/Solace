#!/usr/bin/env python3
"""Restore manifest-listed resources from an explicitly supplied local DLL.

The fixed manifests shipped with this project identify the required DLL and
every byte range. Nothing is downloaded or executed. All resources and existing
destination files are verified before any destination file is created.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import ntpath
from pathlib import Path, PurePosixPath, PureWindowsPath
import sys
import tempfile

from generate_assets import AssetError, address, digest, integer, string, unique_object, verify


DEFAULT_RESOURCES = Path(__file__).resolve().parents[1] / "resources"
MANIFEST_PATHS = (DEFAULT_RESOURCES / "manifest.json",
                  DEFAULT_RESOURCES / "binary" / "manifest.json")


def resource_path(value: object, label: str) -> str:
    path = string(value, label)
    relative = PurePosixPath(path)
    # ntpath.isreserved replaces pathlib's API in Python 3.13; the project
    # also supports Python 3.10-3.12.
    reserved = (ntpath.isreserved(path) if sys.version_info >= (3, 13) else
                any(PureWindowsPath(part).is_reserved() for part in relative.parts))
    if (not relative.parts or relative.is_absolute() or "\\" in path or ":" in path
            or ".." in relative.parts or path != relative.as_posix()
            or reserved
            or any(part.endswith((".", " ")) for part in relative.parts)):
        raise AssetError(f"{label}: expected canonical relative resource path: {path!r}")
    return path


def destination(root: Path, relative: str) -> Path:
    result = (root / relative).resolve()
    if not result.is_relative_to(root):
        raise AssetError(f"resource path escapes destination directory: {relative}")
    for parent in result.parents:
        if parent.exists() and not parent.is_dir():
            raise AssetError(f"resource parent is not a directory: {parent}")
        if parent == root:
            break
    return result


def check_existing(path: Path, payload: bytes) -> bool:
    if not path.exists():
        return False
    if not path.is_file() or path.read_bytes() != payload:
        raise AssetError(f"refusing to replace existing resource with different contents: {path}")
    return True


def prepare(dll: Path, resources: Path) -> tuple[int, int, int]:
    dll = dll.resolve(strict=True)
    if not dll.is_file():
        raise AssetError(f"DLL must be a regular local file: {dll}")
    source = dll.read_bytes()
    source_digest = hashlib.sha256(source).hexdigest()
    root = resources.resolve()
    if root.exists() and not root.is_dir():
        raise AssetError(f"resources destination is not a directory: {root}")

    records: list[tuple[str, bytes]] = []
    manifest_blobs: list[tuple[str, bytes, bool]] = []
    paths = {"manifest.json", "binary/manifest.json"}
    evidence_ids: set[str] = set()
    for manifest_path, prefix, binary in (
        (MANIFEST_PATHS[0], "", False), (MANIFEST_PATHS[1], "binary/", True)
    ):
        blob = manifest_path.read_bytes()
        manifest = json.loads(blob.decode("utf-8-sig"), object_pairs_hook=unique_object)
        if not isinstance(manifest, dict):
            raise AssetError(f"{manifest_path}: manifest must be an object")
        expected_digest = digest(manifest.get("target_sha256"), "target_sha256")
        if source_digest != expected_digest:
            raise AssetError(f"DLL SHA-256 mismatch: expected {expected_digest}, found {source_digest}")
        assets = manifest.get("assets")
        if not isinstance(assets, list) or not assets:
            raise AssetError(f"{manifest_path}: assets must be a nonempty array")
        for index, item in enumerate(assets):
            label = f"{manifest_path.name}.assets[{index}]"
            if not isinstance(item, dict):
                raise AssetError(f"{label}: expected object")
            path = prefix + resource_path(item.get("path"), f"{label}.path")
            if path.casefold() in paths:
                raise AssetError(f"duplicate or reserved resource path: {path}")
            paths.add(path.casefold())
            evidence_id = string(item.get("evidence_id"), f"{label}.evidence_id")
            if evidence_id in evidence_ids:
                raise AssetError(f"duplicate evidence ID across asset manifests: {evidence_id}")
            evidence_ids.add(evidence_id)
            offset = address(item.get("file_offset"), f"{path}.file_offset")
            length = integer(item.get("length"), f"{path}.length")
            end = address(item.get("file_end_exclusive"), f"{path}.file_end_exclusive")
            if offset + length != end or end > len(source):
                raise AssetError(f"{path}: invalid DLL byte range")
            if not binary and (end == len(source) or source[end] != 0):
                raise AssetError(f"{path}: missing original NUL terminator")
            payload = source[offset:end]
            expected = digest(item.get("sha256"), f"{path}.sha256")
            actual = hashlib.sha256(payload).hexdigest()
            if actual != expected:
                raise AssetError(f"{path}: SHA-256 mismatch: expected {expected}, found {actual}")
            records.append((path, payload))
        manifest_blobs.append((prefix + "manifest.json", blob, binary))

    for path in paths:
        if any(parent.as_posix() in paths for parent in PurePosixPath(path).parents
               if parent.as_posix() != "."):
            raise AssetError(f"resource path conflicts with another file: {path}")

    # Reuse the build generator's complete schema, aggregate, encoding, and hash
    # validation on temporary files before touching the requested destination.
    with tempfile.TemporaryDirectory(prefix="solace-resource-validation-") as temporary:
        staged = Path(temporary)
        for path, blob, _ in manifest_blobs:
            target = staged / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(blob)
        for path, payload in records:
            target = staged / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(payload)
        for path, _, binary in manifest_blobs:
            verify(staged / path, binary=binary)

    pending: list[tuple[str, bytes]] = []
    for path, payload in records:
        target = destination(root, path)
        if target == dll:
            raise AssetError("a resource destination must not be the original DLL")
        if not check_existing(target, payload):
            pending.append((path, payload))

    created = 0
    for path, payload in pending:
        target = destination(root, path)
        target.parent.mkdir(parents=True, exist_ok=True)
        # Exclusive creation also protects a file created after preflight.
        try:
            stream = target.open("xb")
        except FileExistsError:
            check_existing(target, payload)
            continue
        try:
            with stream:
                stream.write(payload)
        except BaseException:
            target.unlink(missing_ok=True)
            raise
        created += 1
    return len(records), sum(len(payload) for _, payload in records), created


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dll", type=Path, required=True,
                        help="local DLL with the SHA-256 recorded in both manifests")
    parser.add_argument("--resources", type=Path, default=DEFAULT_RESOURCES,
                        help="destination directory (default: this project's resources)")
    args = parser.parse_args()
    try:
        count, size, created = prepare(args.dll, args.resources)
    except (AssetError, OSError, UnicodeError, json.JSONDecodeError) as error:
        print(f"resource preparation failed: {error}", file=sys.stderr)
        return 1
    print(f"Verified {count} resources / {size} bytes; created {created}, "
          f"already identical {count - created}; destination {args.resources.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
