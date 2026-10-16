"""Validate local resource extraction without requiring the original DLL."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import prepare_resources
from generate_assets import AssetError


class PrepareResourcesTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.dll = self.root / "input.dll"
        self.dll.write_bytes(b"text\0\x89PNG\xff")
        self.output = self.root / "output"
        self.manifests = (self.root / "manifest.json", self.root / "binary.json")
        self.documents = []
        for path, offset, length, kind, encoding in (
            ("sources/example.txt", 0, 4, "text", "utf-8"),
            ("example.bin", 5, 5, "binary", "binary"),
        ):
            record = dict(path=path, file_offset=hex(offset), length=length,
                          file_end_exclusive=hex(offset + length), kind=kind,
                          evidence_id=kind, encoding=encoding, template=False,
                          rva=hex(offset), section=".rdata",
                          sha256=hashlib.sha256(self.dll.read_bytes()[offset:offset + length]).hexdigest())
            if encoding == "utf-8":
                record["nul_terminator_offset"] = hex(offset + length)
            self.documents.append(dict(target="input.dll",
                                       target_sha256=hashlib.sha256(self.dll.read_bytes()).hexdigest(),
                                       total_assets=1, total_bytes=length,
                                       counts={kind: 1}, assets=[record]))
        self.save_manifests()
        self.manifest_patch = patch.object(prepare_resources, "MANIFEST_PATHS", self.manifests)
        self.manifest_patch.start()
        self.addCleanup(self.manifest_patch.stop)

    def save_manifests(self) -> None:
        for path, document in zip(self.manifests, self.documents):
            path.write_text(json.dumps(document), encoding="utf-8")

    def test_extracts_exact_bytes_and_preserves_identical_files(self) -> None:
        self.assertEqual(prepare_resources.prepare(self.dll, self.output), (2, 9, 2))
        self.assertEqual((self.output / "sources/example.txt").read_bytes(), b"text")
        binary = self.output / "binary/example.bin"
        self.assertEqual(binary.read_bytes(), b"\x89PNG\xff")
        modified = binary.stat().st_mtime_ns
        self.assertEqual(prepare_resources.prepare(self.dll, self.output), (2, 9, 0))
        self.assertEqual(binary.stat().st_mtime_ns, modified)

    def test_rejects_source_hash_before_writing(self) -> None:
        self.dll.write_bytes(b"different DLL")
        with self.assertRaisesRegex(AssetError, "DLL SHA-256 mismatch"):
            prepare_resources.prepare(self.dll, self.output)
        self.assertFalse(self.output.exists())

    def test_rejects_later_asset_hash_before_writing(self) -> None:
        self.documents[1]["assets"][0]["sha256"] = "0" * 64
        self.save_manifests()
        with self.assertRaisesRegex(AssetError, "SHA-256 mismatch"):
            prepare_resources.prepare(self.dll, self.output)
        self.assertFalse(self.output.exists())

    def test_rejects_path_traversal_before_writing(self) -> None:
        for path in ("../escaped.txt", "/absolute.txt", "C:/absolute.txt", "sources/../x", "x\\y",
                     "sources/NUL.txt", "CON/example.txt", "sources/trailing."):
            with self.subTest(path=path):
                self.documents[0]["assets"][0]["path"] = path
                self.save_manifests()
                with self.assertRaisesRegex(AssetError, "canonical relative resource path"):
                    prepare_resources.prepare(self.dll, self.output)
                self.assertFalse(self.output.exists())

    def test_rejects_range_outside_dll_before_writing(self) -> None:
        self.documents[1]["assets"][0].update(length=6, file_end_exclusive="0xb")
        self.save_manifests()
        with self.assertRaisesRegex(AssetError, "invalid DLL byte range"):
            prepare_resources.prepare(self.dll, self.output)
        self.assertFalse(self.output.exists())

    def test_reuses_full_manifest_validation_before_writing(self) -> None:
        self.documents[1]["total_bytes"] = 100
        self.save_manifests()
        with self.assertRaisesRegex(AssetError, "total_bytes"):
            prepare_resources.prepare(self.dll, self.output)
        self.assertFalse(self.output.exists())

    def test_rejects_different_existing_file_before_writing_earlier_asset(self) -> None:
        existing = self.output / "binary/example.bin"
        existing.parent.mkdir(parents=True)
        existing.write_bytes(b"keep me")
        with self.assertRaisesRegex(AssetError, "different contents"):
            prepare_resources.prepare(self.dll, self.output)
        self.assertEqual(existing.read_bytes(), b"keep me")
        self.assertFalse((self.output / "sources/example.txt").exists())


if __name__ == "__main__":
    unittest.main()
