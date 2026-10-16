#!/usr/bin/env python3
"""Verify current build evidence and create a source-only reconstruction package.

No compilation, target execution, downloads, source edits, or original-DLL
loading occurs here. BUILD_PROOF.json and a new archive are the only outputs.
"""
from __future__ import annotations

import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile

ORIGINAL_SHA256 = "8e60e1071acd82b08dfd2bbc41b1155e10cfd113075f0ff40ed6372c2f5f232b"
EXPECTED_TESTS = {"dll_api", "config", "assets", "bootstrap", "script", "script_utils", "shaders",
                  "script_math", "script_draw_values", "script_callbacks", "script_services", "script_session",
                  "lua_utf8_io", "script_cli"}
EXPECTED_OPERATIONS = {"Lerp", "Clamp", "RemapVal", "RemapValClamped", "Fnv1a"}
EXCLUDED_DIRECTORIES = {".git", ".vs", "__pycache__", "upstream", "velocity", "configs", "profiles"}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def record(path: Path) -> dict:
    require(path.is_file(), f"required file is missing: {path}")
    return {"bytes": path.stat().st_size, "sha256": sha256(path)}


def read_json(path: Path) -> dict:
    with path.open(encoding="utf-8-sig") as source:
        value = json.load(source)
    require(isinstance(value, dict), f"expected JSON object: {path}")
    return value


def assert_hash(path: Path, expected: object, label: str) -> dict:
    result = record(path)
    require(isinstance(expected, str) and result["sha256"] == expected.lower(),
            f"{label} hash does not match the current file: {path}")
    return result


def verify_tests(path: Path) -> dict:
    root = ET.parse(path).getroot()
    cases = list(root.iter("testcase"))
    names = [case.get("name") for case in cases]
    require(len(cases) == len(EXPECTED_TESTS) and set(names) == EXPECTED_TESTS,
            f"test XML must contain exactly the {len(EXPECTED_TESTS)} expected tests; found {names}")
    for node in root.iter():
        require(node.tag not in {"failure", "error", "skipped"},
                f"test XML contains {node.tag}")
        for attribute in ("failures", "errors", "skipped", "disabled"):
            if attribute in node.attrib:
                require(int(node.attrib[attribute]) == 0,
                        f"test XML reports nonzero {attribute}")
    for case in cases:
        require(case.get("status", "run") in {"run", "passed"},
                f"test was not run: {case.get('name')}")
    return {**record(path), "count": len(cases), "failures": 0, "skipped": 0,
            "tests": [{"name": case.get("name"), "seconds": case.get("time"),
                       "output": case.findtext("system-out", "")} for case in cases]}


def verify_machine_report(project: Path, build: Path) -> dict:
    path = project / "evidence/script-machine-code.json"
    data = read_json(path)
    require(data.get("dll_sha256") == ORIGINAL_SHA256, "machine report has the wrong original DLL identity")
    tool = build / "Release/solace-tool.exe"
    native = build / "Release/native_float_probe.exe"
    require(Path(data["tool"]).resolve() == tool.resolve(), "machine report does not identify the current Release tool")
    tool_record = assert_hash(tool, data.get("tool_sha256"), "machine report tool")
    verifier_record = assert_hash(project / "tools/verify_script_machine_code.py",
                                  data.get("verifier_sha256"), "machine-code verifier")
    native_data = data.get("native_verification")
    require(isinstance(native_data, dict), "machine report lacks native CPU calibration")
    require(Path(native_data["path"]).resolve() == native.resolve(),
            "machine report does not identify the current Release native probe")
    native_record = assert_hash(native, native_data.get("sha256"), "native CPU probe")
    comparisons = data.get("comparisons")
    require(isinstance(comparisons, list) and comparisons, "machine report has no comparisons")
    require(len(comparisons) == data.get("total"), "machine report comparison count is inconsistent")
    require([row.get("case") for row in comparisons] == list(range(len(comparisons))),
            "machine report comparison IDs are missing, repeated, or unordered")
    actual_counts = dict(Counter(row["request"]["op"] for row in comparisons))
    require(set(actual_counts) == EXPECTED_OPERATIONS and actual_counts == data.get("by_operation"),
            "machine report operation coverage is inconsistent")
    exact = 0
    calibrated = 0
    for row in comparisons:
        key = "value" if row["request"]["op"] == "Fnv1a" else "bits"
        same = row["original"][key] == row["rebuilt"][key]
        require(row.get("exact_match") is same, "machine report contains an incorrect exact-match flag")
        if same:
            exact += 1
            continue
        require(key == "bits" and row.get("classification") == "nan-payload-or-sign-difference",
                "machine report contains an unresolved numeric difference")
        for result in (row["original"][key], row["rebuilt"][key]):
            require(type(result) is int and result & 0x7F800000 == 0x7F800000 and result & 0x007FFFFF != 0,
                    "machine report misclassifies a non-NaN value difference")
        require(row.get("native_matches_rebuilt") is True and
                row.get("native_original_order", {}).get("bits") == row["rebuilt"]["bits"],
                "machine report has an unconfirmed NaN difference")
        calibrated += 1
    require(data.get("unresolved_mismatches") == 0 and data.get("exact_matches") == exact and
            data.get("mismatches") == calibrated and data.get("confirmed_emulator_nan_differences") == calibrated,
            "machine report totals are inconsistent or unresolved")
    require(native_data.get("cases") == calibrated and
            native_data.get("confirmed_emulator_nan_differences") == calibrated,
            "native probe coverage count is inconsistent")
    return {**record(path), "total": len(comparisons), "exact_matches": exact,
            "raw_nan_differences": calibrated, "native_confirmed_differences": calibrated,
            "unresolved_mismatches": 0, "by_operation": actual_counts,
            "tool": tool_record, "native_probe": native_record, "verifier": verifier_record}


def verify_config(build: Path) -> dict:
    path = build / "config-validation.json"
    data = read_json(path)
    require(data.get("status") == "passed" and data.get("exit_code") == 0,
            "actual-configuration validation did not pass")
    require(data.get("originals_unchanged") is True, "actual configurations were modified")
    exe_record = assert_hash(build / "Release/config_tests.exe", data.get("exe_sha256"), "configuration test")
    profiles = data.get("profiles")
    require(isinstance(profiles, list) and profiles and len(profiles) == data.get("profile_count"),
            "configuration validation profile count is inconsistent")
    total_fields = 0
    for profile in profiles:
        require(set(profile) == {"sha256_before", "sha256_after", "field_count"},
                "configuration summary must contain only hashes and field counts")
        before, after = profile["sha256_before"], profile["sha256_after"]
        require(isinstance(before, str) and len(before) == 64 and all(c in "0123456789abcdef" for c in before),
                "configuration summary contains an invalid SHA-256")
        require(before == after, "an actual configuration changed during validation")
        count = profile["field_count"]
        require(type(count) is int and count >= 0, "configuration field count is invalid")
        total_fields += count
    require(total_fields == data.get("total_field_occurrences"), "configuration field total is inconsistent")
    require(isinstance(data.get("stdout"), str) and data["stdout"], "configuration validation lacks actual tool output")
    return {**record(path), "status": "passed", "profile_count": len(profiles),
            "total_field_occurrences": total_fields, "originals_unchanged": True,
            "profiles": profiles, "test_executable": exe_record}


def verify_shaders(build: Path) -> tuple[dict, list[tuple[str, Path]]]:
    directory = (build / "compiled-shaders").resolve()
    path = directory / "manifest.json"
    data = read_json(path)
    require(data.get("source_sha256") == ORIGINAL_SHA256, "shader manifest has the wrong source identity")
    require(data.get("status") == "shader-compilation-verified-rendering-not-verified",
            "shader manifest has an unexpected completion claim")
    programs = data.get("programs")
    require(isinstance(programs, list) and len(programs) == 17, "expected 17 compiled shader programs")
    names = set()
    outputs = []
    archive_files = [("verification/compiled-shaders/manifest.json", path)]
    for program in programs:
        name = program.get("output")
        require(isinstance(name, str) and Path(name).name == name and name.endswith(".cso") and name not in names,
                "shader manifest has an unsafe or repeated output name")
        names.add(name)
        compiled = directory / name
        require(compiled.resolve().parent == directory, "shader output leaves its directory")
        info = record(compiled)
        require(info["bytes"] > 0 and info["bytes"] == program.get("bytecode_size"),
                f"shader size does not match the manifest: {name}")
        if "sha256" in program:
            require(info["sha256"] == program["sha256"], f"shader hash mismatch: {name}")
        outputs.append({"output": name, "source": program["source"], "entrypoint": program["entrypoint"],
                        "target": program["target"], **info})
        archive_files.append(("verification/compiled-shaders/" + name, compiled))
    return ({**record(path), "program_count": len(outputs), "rendering_verified": False, "programs": outputs}, archive_files)


def source_files(project: Path, archive: Path, original: Path) -> list[tuple[str, Path]]:
    included = []
    for current, directories, files in os.walk(project):
        current_path = Path(current)
        relative_parent = current_path.relative_to(project)
        selected = []
        for name in directories:
            relative = relative_parent / name
            folded = name.casefold()
            if folded.startswith("build") or folded in EXCLUDED_DIRECTORIES or relative.as_posix().casefold() == "tools/binary_asset_deps":
                continue
            require(not (current_path / name).is_symlink(), f"source package refuses directory symlink: {relative}")
            selected.append(name)
        directories[:] = sorted(selected)
        for name in sorted(files):
            path = current_path / name
            relative = path.relative_to(project)
            if relative.as_posix() == "BUILD_PROOF.json" or path.resolve() == archive:
                continue
            if relative.parent == Path(".") and path.suffix.casefold() == ".zip":
                continue
            require(not path.is_symlink(), f"source package refuses file symlink: {relative}")
            require(path.resolve() != original and name.casefold() != "solace.dll",
                    f"original DLL must not be inside the source package: {relative}")
            included.append((relative.as_posix(), path))
    return sorted(included)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--build-directory", type=Path, default=Path("build"))
    parser.add_argument("--original", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    args = parser.parse_args()
    project = args.project.resolve(strict=True)
    build = (args.build_directory if args.build_directory.is_absolute() else project / args.build_directory).resolve(strict=True)
    original = args.original.resolve(strict=True)
    archive = args.archive.resolve()
    require(not archive.exists(), f"archive already exists; refusing to overwrite: {archive}")
    require(archive.suffix.casefold() == ".zip", "archive must use a .zip extension")
    original_record = assert_hash(original, ORIGINAL_SHA256, "original DLL")
    tests = verify_tests(build / "test-results.xml")
    machine = verify_machine_report(project, build)
    configurations = verify_config(build)
    shaders, shader_files = verify_shaders(build)
    build_log = build / "build-latest.log"
    require(record(build_log)["bytes"] > 0, "build log is empty")

    package_files = source_files(project, archive, original)
    require(any(name == "CMakeLists.txt" for name, _ in package_files), "project has no CMakeLists.txt")
    require(any(name == "evidence/script-machine-code.json" for name, _ in package_files),
            "machine-code evidence would be missing from the source package")
    verification_files = [
        ("verification/test-results.xml", build / "test-results.xml"),
        ("verification/config-validation.json", build / "config-validation.json"),
        ("verification/build-latest.log", build_log),
        *shader_files,
    ]
    inventory = []
    for name, path in package_files + verification_files:
        info = record(path)
        require(info["sha256"] != ORIGINAL_SHA256, f"original DLL bytes found in package input: {name}")
        inventory.append({"path": name, **info})
    require(len({item["path"].casefold() for item in inventory}) == len(inventory),
            "source and verification paths collide in the archive")
    binaries = []
    for path in sorted((build / "Release").iterdir()):
        if path.is_file() and path.suffix.lower() in {".exe", ".dll", ".lib"}:
            binaries.append({"name": path.name, **record(path)})
    proof = {
        "schema_version": 1, "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "status": "partial-reconstruction-verified", "full_application_complete": False,
        "original_dll": original_record, "configuration": "Release",
        "limits": ["This is a source package for the recovered core, not the complete Solace application.",
                   "Shader compilation and reflection do not establish rendering or game-host compatibility.",
                   "Original DLL, upstream Velocity source, build products, and personal configuration files are excluded."],
        "tests": tests, "machine_code": machine, "actual_configurations": configurations,
        "shaders": shaders, "current_release_binaries": binaries,
        "source_file_count": len(package_files), "verification_file_count": len(verification_files),
        "files": inventory,
    }
    proof_bytes = (json.dumps(proof, indent=2, ensure_ascii=False) + "\n").encode("utf-8")
    expected_hashes = {item["path"]: item["sha256"] for item in inventory}
    archive.parent.mkdir(parents=True, exist_ok=True)
    created = False
    try:
        with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as output:
            created = True
            output.writestr("BUILD_PROOF.json", proof_bytes)
            for name, path in package_files + verification_files:
                contents = path.read_bytes()
                require(hashlib.sha256(contents).hexdigest() == expected_hashes[name],
                        f"package input changed during packaging: {name}")
                output.writestr(name, contents)
        # The proof is a generated output. All source/evidence inputs remain untouched.
        with tempfile.NamedTemporaryFile(dir=project, prefix="BUILD_PROOF.", suffix=".tmp", delete=False) as output:
            temporary = Path(output.name)
            output.write(proof_bytes)
        try:
            os.replace(temporary, project / "BUILD_PROOF.json")
        finally:
            temporary.unlink(missing_ok=True)
    except BaseException:
        if created:
            archive.unlink(missing_ok=True)
        raise
    print(json.dumps({"status": proof["status"], "full_application_complete": False,
                      "source_files": len(package_files), "verification_files": len(verification_files),
                      "archive": str(archive), "archive_sha256": sha256(archive)}, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, TypeError, ET.ParseError, zipfile.BadZipFile) as error:
        print(f"packaging failed: {error}", file=sys.stderr)
        raise SystemExit(1)
