#!/usr/bin/env python3
"""Compare rebuilt Lua helpers with original x64 instructions in Unicorn.

The DLL is parsed as data, mapped only into emulator memory, and never loaded
into Windows or another process. Only Lua argument/result boundaries are hooked.
"""
from __future__ import annotations

import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import itertools
import json
from pathlib import Path
import random
import struct
import subprocess
import sys

import pefile
import unicorn
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UC_HOOK_CODE, UC_PROT_ALL
from unicorn.x86_const import (
    UC_X86_REG_RAX, UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_R8,
    UC_X86_REG_RIP, UC_X86_REG_RSP, UC_X86_REG_XMM0, UC_X86_REG_XMM1,
    UC_X86_REG_MXCSR,
)

EXPECTED_SHA256 = "8e60e1071acd82b08dfd2bbc41b1155e10cfd113075f0ff40ed6372c2f5f232b"
BASE = 0x180000000
FUNCTIONS = {
    "Lerp": (0x18028AE20, 0x18028AE95),
    "Clamp": (0x18028B030, 0x18028B0BC),
    "RemapVal": (0x18028B0C0, 0x18028B199),
    "RemapValClamped": (0x18028B1A0, 0x18028B29C),
    "Fnv1a": (0x180290320, 0x180290417),
}
CHECK_NUMBER = 0x180380230
PUSH_NUMBER = 0x18037D110
CHECK_STRING = 0x180380110
PUSH_INTEGER = 0x18037D130
STACK = 0x70000000
STACK_SIZE = 0x20000
HEAP = 0x40000000
RETURN = 0x60000000
SEED = 0x501ACE


def align(value: int) -> int:
    return (value + 0xFFF) & ~0xFFF


def bits_float(value: int) -> float:
    return struct.unpack("<f", struct.pack("<I", value))[0]


def float_bits(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", value))[0]


def nan_bits(value: int) -> bool:
    return value & 0x7F800000 == 0x7F800000 and value & 0x007FFFFF != 0


class OriginalMachineCode:
    def __init__(self, dll: Path):
        data = dll.read_bytes()
        self.sha256 = hashlib.sha256(data).hexdigest()
        if self.sha256 != EXPECTED_SHA256:
            raise ValueError(f"wrong DLL identity: expected {EXPECTED_SHA256}, found {self.sha256}")
        self.pe = pefile.PE(data=data)
        if self.pe.OPTIONAL_HEADER.ImageBase != BASE:
            raise ValueError("unexpected preferred PE image base")
        self.uc = Uc(UC_ARCH_X86, UC_MODE_64)
        self.uc.mem_map(BASE, align(self.pe.OPTIONAL_HEADER.SizeOfImage), UC_PROT_ALL)
        self.uc.mem_write(BASE, self.pe.get_memory_mapped_image())
        self.uc.mem_map(STACK, STACK_SIZE)
        self.uc.mem_map(HEAP, 0x20000)
        self.uc.mem_map(RETURN, 0x1000)
        self.uc.mem_write(RETURN, b"\xC3")
        self.uc.reg_write(UC_X86_REG_MXCSR, 0x1F80)
        self.baseline = self.uc.context_save()
        self.uc.hook_add(UC_HOOK_CODE, self.on_instruction)
        self.code_sha256 = {
            name: hashlib.sha256(bytes(self.uc.mem_read(begin, end - begin))).hexdigest()
            for name, (begin, end) in FUNCTIONS.items()
        }

    def return_from_lua(self) -> None:
        rsp = self.uc.reg_read(UC_X86_REG_RSP)
        target = struct.unpack("<Q", self.uc.mem_read(rsp, 8))[0]
        self.uc.reg_write(UC_X86_REG_RSP, rsp + 8)
        self.uc.reg_write(UC_X86_REG_RIP, target)

    def on_instruction(self, uc, address, size, user_data) -> None:
        self.instructions += 1
        if address == RETURN:
            self.returned = True
            uc.emu_stop()
            return
        if address == CHECK_NUMBER:
            index = uc.reg_read(UC_X86_REG_RDX) & 0xFFFFFFFF
            if self.request["op"] == "Fnv1a" or not 1 <= index <= len(self.request["bits"]):
                raise RuntimeError(f"unexpected luaL_checknumber index {index}")
            self.checked.append(index)
            # Python's float is binary64: this supplies exactly the Lua numeric
            # boundary, including the CPU's float-to-double NaN conversion.
            double = bits_float(self.request["bits"][index - 1])
            uc.reg_write(UC_X86_REG_XMM0, struct.unpack("<Q", struct.pack("<d", double))[0])
            self.return_from_lua()
            return
        if address == PUSH_NUMBER:
            if self.request["op"] == "Fnv1a" or self.result is not None:
                raise RuntimeError("unexpected lua_pushnumber")
            double_bits = uc.reg_read(UC_X86_REG_XMM1) & ((1 << 64) - 1)
            double = struct.unpack("<d", struct.pack("<Q", double_bits))[0]
            self.result = {"bits": float_bits(double)}
            self.return_from_lua()
            return
        if address == CHECK_STRING:
            if self.request["op"] != "Fnv1a" or uc.reg_read(UC_X86_REG_RDX) != 1:
                raise RuntimeError("unexpected luaL_checklstring")
            self.checked.append(1)
            length_pointer = uc.reg_read(UC_X86_REG_R8)
            uc.mem_write(length_pointer, struct.pack("<Q", len(self.payload)))
            uc.reg_write(UC_X86_REG_RAX, HEAP + 0x1000)
            self.return_from_lua()
            return
        if address == PUSH_INTEGER:
            if self.request["op"] != "Fnv1a" or self.result is not None:
                raise RuntimeError("unexpected lua_pushinteger")
            self.result = {"value": uc.reg_read(UC_X86_REG_RDX)}
            self.return_from_lua()
            return
        begin, end = FUNCTIONS[self.request["op"]]
        if not begin <= address < end:
            raise RuntimeError(f"unexpected execution outside selected helper: 0x{address:x}")

    def run(self, request: dict) -> dict:
        self.uc.context_restore(self.baseline)
        self.request = request
        self.result = None
        self.checked = []
        self.returned = False
        self.instructions = 0
        self.payload = bytes.fromhex(request.get("hex", ""))
        if len(self.payload) > 0x10000:
            raise ValueError("test payload exceeds emulator input allocation")
        self.uc.mem_write(HEAP + 0x1000, self.payload + b"\0")
        rsp = STACK + STACK_SIZE - 0x1008
        self.uc.mem_write(rsp - 0x200, b"\0" * 0x400)
        self.uc.mem_write(rsp, struct.pack("<Q", RETURN))
        self.uc.reg_write(UC_X86_REG_RSP, rsp)
        self.uc.reg_write(UC_X86_REG_RCX, HEAP)
        # An instruction budget bounds every run. A per-call timer would create
        # thousands of redundant host timer threads on Windows.
        self.uc.emu_start(FUNCTIONS[request["op"]][0], RETURN + 1, count=250_000)
        expected_indices = [1] if request["op"] == "Fnv1a" else list(range(1, len(request["bits"]) + 1))
        if not self.returned or self.result is None:
            raise RuntimeError(f"helper failed to return one value: {request}")
        if self.uc.reg_read(UC_X86_REG_RAX) != 1 or self.checked != expected_indices:
            raise RuntimeError(f"unexpected Lua helper contract: {request}")
        return self.result


def test_cases() -> list[dict]:
    result = []
    seen = set()

    def add(op: str, label: str, bits: list[int] | None = None, payload: bytes | None = None):
        request = {"op": op, "hex": payload.hex()} if payload is not None else {"op": op, "bits": bits}
        identity = json.dumps(request, sort_keys=True)
        if identity not in seen:
            seen.add(identity)
            result.append({"label": label, "request": request})

    special = [0x00000000, 0x80000000, 0x3F800000, 0xBF800000,
               0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000,
               0x7FC12345, 0xFFC54321, 0x7F800001, 0xFF800001,
               0x00000001, 0x80000001, 0x007FFFFF, 0x807FFFFF,
               0x00800000, 0x80800000, 0x7F7FFFFF, 0xFF7FFFFF,
               0x3F7FFFFF, 0x3F800001, 0x4B800000, 0x4B800001]
    rng = random.Random(SEED)
    for op in ("Lerp", "Clamp", "RemapVal", "RemapValClamped"):
        baseline = [float_bits(x) for x in ([0.25, 0.0, 1.0] if op in ("Lerp", "Clamp") else [0.25, 0.0, 1.0, -3.0, 7.0])]
        for index in range(len(baseline)):
            for value in special:
                inputs = baseline.copy()
                inputs[index] = value
                add(op, "special-value-in-each-argument", inputs)
        for _ in range(768):
            add(op, "seeded-random-float-bit-patterns", [rng.getrandbits(32) for _ in baseline])
        for _ in range(128):
            add(op, "seeded-finite-rounding", [float_bits(rng.uniform(-1e15, 1e15)) for _ in baseline])
    for op in ("Lerp", "Clamp"):
        for values in itertools.product([0x00000000, 0x80000000, 0x3F800000, 0xBF800000,
                                         0x7FC12345, 0xFFC54321, 0x7F800000, 0xFF800000], repeat=3):
            add(op, "signed-zero-nan-infinity-cross-product", list(values))
    for op in ("RemapVal", "RemapValClamped"):
        for value, out_first, out_last in itertools.product(
                [0x3F000000, 0x7FC12345],
                [0x7FC12345, 0xFFC54321, 0x7F800000, 0xFF800000],
                [0x7FC12345, 0xFFC54321, 0x7F800000, 0xFF800000]):
            add(op, "multiple-nan-payload-propagation", [value, 0x00000000, 0x3F800000, out_first, out_last])
        for first in (0.0, -0.0, 1.0, -1.0, 16777216.0):
            for value in (-5.0, -0.0, 0.0, 0.5, 1.0, 5.0):
                for pair in ((first, first), (1.0, -1.0), (-1.0, 1.0)):
                    for out_pair in ((0.0, 1.0), (1.0, 0.0), (-0.0, 0.0), (1.0, 1.0)):
                        add(op, "equal-reversed-ranges-and-extrapolation", [float_bits(x) for x in (value, *pair, *out_pair)])
        for input_first in (0x3F7FFFFF, 0x3F800000, 0x3F800001, 0x007FFFFF, 0x00800000):
            for value in (input_first - 1, input_first, input_first + 1, input_first + 2):
                add(op, "one-ulp-input-ranges", [value, input_first, input_first + 1, 0x3F800000, 0x3F800001])
    for length in range(33):
        add("Fnv1a", "four-byte-loop-and-remainder-boundaries",
            payload=bytes((index * 37 + 128) & 255 for index in range(length)))
    for payload in (b"", b"a", b"\0", b"a\0b", bytes(range(256)), "Solace\u4e2d\u6587".encode("utf-8")):
        add("Fnv1a", "empty-embedded-nul-unsigned-bytes-utf8", payload=payload)
    for _ in range(256):
        add("Fnv1a", "seeded-random-byte-strings", payload=bytes(rng.getrandbits(8) for _ in range(rng.randrange(0, 1025))))
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dll", required=True, type=Path)
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--native-probe", type=Path,
                        help="optional independently compiled original-order SSE probe for raw NaN differences")
    args = parser.parse_args()
    machine = OriginalMachineCode(args.dll)
    cases = test_cases()
    requests = "".join(json.dumps(case["request"], separators=(",", ":")) + "\n" for case in cases)
    process = subprocess.run([str(args.tool.resolve()), "numeric-check"], input=requests,
                             capture_output=True, text=True, timeout=180, check=False)
    if process.returncode:
        raise RuntimeError(f"numeric-check failed ({process.returncode}): {process.stderr}")
    outputs = [json.loads(line) for line in process.stdout.splitlines()]
    if len(outputs) != len(cases):
        raise RuntimeError(f"numeric-check returned {len(outputs)} rows for {len(cases)} cases")
    print(f"Rebuilt Lua runtime returned {len(outputs)} rows; emulating original instructions.", flush=True)
    comparisons = []
    for index, (case, actual) in enumerate(zip(cases, outputs)):
        expected = machine.run(case["request"])
        key = "value" if case["request"]["op"] == "Fnv1a" else "bits"
        if key not in actual or type(actual[key]) is not int:
            raise RuntimeError(f"malformed numeric-check output at case {index}: {actual}")
        equal = actual[key] == expected[key]
        classification = "exact" if equal else (
            "nan-payload-or-sign-difference" if key == "bits" and nan_bits(actual[key]) and nan_bits(expected[key])
            else "value-or-signed-zero-difference")
        comparisons.append(dict(case=index, **case, original=expected, rebuilt=actual,
                                exact_match=equal, classification=classification,
                                original_instruction_count=machine.instructions))
    mismatches = [row for row in comparisons if not row["exact_match"]]
    native_verification = None
    confirmed_emulator_differences = []
    if args.native_probe and mismatches:
        candidates = [row for row in mismatches
                      if row["classification"] == "nan-payload-or-sign-difference"
                      and row["request"]["op"] in ("Lerp", "RemapVal", "RemapValClamped")]
        probe_input = "".join(row["request"]["op"] + " " +
                              " ".join(str(value) for value in row["request"]["bits"]) + "\n"
                              for row in candidates)
        probe = subprocess.run([str(args.native_probe.resolve()), "--batch"], input=probe_input,
                               capture_output=True, text=True, timeout=30, check=True)
        native_results = [json.loads(line) for line in probe.stdout.splitlines()]
        if len(native_results) != len(candidates):
            raise RuntimeError("native probe returned an unexpected number of results")
        for row, native_result in zip(candidates, native_results):
            if "bits" not in native_result or type(native_result["bits"]) is not int:
                raise RuntimeError("malformed native probe result")
            row["native_original_order"] = native_result
            row["native_matches_rebuilt"] = native_result["bits"] == row["rebuilt"]["bits"]
            if row["native_matches_rebuilt"]:
                confirmed_emulator_differences.append(row)
        native_verification = {
            "path": str(args.native_probe.resolve()),
            "sha256": hashlib.sha256(args.native_probe.read_bytes()).hexdigest(),
            "cases": len(candidates),
            "confirmed_emulator_nan_differences": len(confirmed_emulator_differences),
            "method": "independent /Od /fp:strict SSE-intrinsic probe; inspected original operand order",
        }
    unresolved = len(mismatches) - len(confirmed_emulator_differences)
    report = {
        "checked_at_utc": datetime.now(timezone.utc).isoformat(),
        "dll": str(args.dll.resolve()), "dll_sha256": machine.sha256,
        "tool": str(args.tool.resolve()), "tool_sha256": hashlib.sha256(args.tool.read_bytes()).hexdigest(),
        "verifier_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "unicorn_version": unicorn.__version__, "pefile_version": pefile.__version__,
        "seed": SEED, "mxcsr": "0x1f80", "input_contract": "binary32 bits widened to Lua binary64; original helper narrows its inputs",
        "execution": "original helper instructions in Unicorn only; DLL never loaded by Windows",
        "hooked_addresses": {"luaL_checknumber": hex(CHECK_NUMBER), "lua_pushnumber": hex(PUSH_NUMBER),
                             "luaL_checklstring": hex(CHECK_STRING), "lua_pushinteger": hex(PUSH_INTEGER)},
        "functions": {name: {"begin": hex(begin), "end_exclusive": hex(end), "bytes_sha256": machine.code_sha256[name]}
                      for name, (begin, end) in FUNCTIONS.items()},
        "total": len(cases), "exact_matches": len(cases) - len(mismatches), "mismatches": len(mismatches),
        "confirmed_emulator_nan_differences": len(confirmed_emulator_differences),
        "unresolved_mismatches": unresolved, "native_verification": native_verification,
        "by_operation": dict(Counter(row["request"]["op"] for row in comparisons)),
        "classifications": dict(Counter(row["classification"] for row in comparisons)),
        "coverage": dict(Counter(row["label"] for row in comparisons)),
        "comparisons": comparisons,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Machine-code comparison: {report['exact_matches']}/{report['total']} exact; {len(mismatches)} mismatches; {args.output}")
    if native_verification:
        print(f"Independent CPU probe: {len(confirmed_emulator_differences)} emulator NaN differences confirmed; {unresolved} unresolved differences.")
    for mismatch in mismatches[:12]:
        print(json.dumps(mismatch, separators=(",", ":")))
    return 1 if unresolved else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, unicorn.UcError) as error:
        print(f"machine-code verification failed: {error}", file=sys.stderr)
        raise SystemExit(2)
