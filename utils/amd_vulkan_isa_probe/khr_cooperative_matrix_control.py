#!/usr/bin/env python3
"""Compile the upstream Khronos cooperative-matrix path and qualify driver ISA.

Compiler-only mode is suitable for ordinary CI. Hardware mode additionally
queries device matrix properties and creates a Vulkan compute pipeline; neither
mode executes the shader or proves numerical correctness.
"""

from __future__ import annotations

import argparse
import json
import re
import struct
import subprocess
from pathlib import Path

SPIRV_MAGIC = 0x07230203
OP_CAPABILITY = 17
OP_COOPERATIVE_MATRIX_TYPE_KHR = 4456
OP_COOPERATIVE_MATRIX_MUL_ADD_KHR = 4459
COOPERATIVE_MATRIX_CAPABILITY_KHR = 6022

CASES = (
    {"name": "f16xf16-f16", "a": "f16", "b": "f16",
     "c": "f16", "r": "f16", "define": "CONTROL_F16=1"},
    {"name": "i32xi32-i32", "a": "i32", "b": "i32",
     "c": "i32", "r": "i32", "define": "CONTROL_F16=0"},
)

NATIVE_MATRIX_INSTRUCTION = re.compile(r"\bv_(?:wmma|swmmac|mfma)[a-z0-9_]*\b", re.I)


def run(argv: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        argv, text=True, encoding="utf-8", errors="replace",
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )


def read_opcodes(path: Path) -> tuple[set[int], bool]:
    data = path.read_bytes()
    if len(data) < 20 or len(data) % 4:
        raise ValueError("truncated or unaligned SPIR-V module")
    words = struct.unpack("<" + "I" * (len(data) // 4), data)
    if words[0] != SPIRV_MAGIC:
        raise ValueError("invalid SPIR-V magic")
    opcodes: set[int] = set()
    has_capability = False
    offset = 5
    while offset < len(words):
        instruction = words[offset]
        count, opcode = instruction >> 16, instruction & 0xFFFF
        if count == 0 or offset + count > len(words):
            raise ValueError("invalid SPIR-V instruction length")
        if opcode == OP_CAPABILITY and count >= 2:
            has_capability |= words[offset + 1] == COOPERATIVE_MATRIX_CAPABILITY_KHR
        opcodes.add(opcode)
        offset += count
    return opcodes, has_capability


def classify_properties(proc: subprocess.CompletedProcess[str]) -> str:
    output = proc.stdout + "\n" + proc.stderr
    if proc.returncode == 0 and "PROPERTY_SUPPORTED" in output:
        return "supported"
    if any(marker in output for marker in (
        "UNSUPPORTED:", "UNSUPPORTED_PROPERTY:", "UNSUPPORTED_FEATURE:"
    )):
        return "unsupported"
    return "failed"


def classify_driver(proc: subprocess.CompletedProcess[str], isa: Path) -> str:
    output = proc.stdout + "\n" + proc.stderr
    if any(marker in output for marker in (
        "UNSUPPORTED:", "UNSUPPORTED_PROPERTY:", "UNSUPPORTED_FEATURE:"
    )):
        return "unsupported"
    if "NO_DISASSEMBLY:" in output:
        return "unsupported"
    if proc.returncode != 0 or not isa.is_file() or isa.stat().st_size == 0:
        return "failed"
    return "completed"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxc", required=True)
    parser.add_argument("--matrix-probe", help="Enable installed-driver qualification")
    parser.add_argument("--spirv-val", help="Validate the generated SPIR-V modules")
    parser.add_argument("--out-dir", default="out/amd-khr-matrix-control")
    parser.add_argument("--mode", choices=("f16xf16-f16", "i32xi32-i32"),
                        action="append")
    args = parser.parse_args()

    dxc = str(Path(args.dxc).resolve())
    matrix_probe = str(Path(args.matrix_probe).resolve()) if args.matrix_probe else None
    validator = str(Path(args.spirv_val).resolve()) if args.spirv_val else None
    shader = Path(__file__).resolve().parent / "shaders/khr_cooperative_matrix.hlsl"
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    cases = [c for c in CASES if not args.mode or c["name"] in args.mode]
    results: list[dict] = []
    failures = 0
    unsupported = 0

    for case in cases:
        stem = case["name"]
        spv = out_dir / (stem + ".spv")
        isa = out_dir / (stem + ".isa")
        spv.unlink(missing_ok=True)
        isa.unlink(missing_ok=True)
        compile_cmd = [
            dxc, "-spirv", "-T", "cs_6_6", "-E", "main",
            "-HV", "2021", "-enable-16bit-types",
            "-fspv-target-env=vulkan1.3", "-D" + case["define"],
            str(shader), "-Fo", str(spv),
        ]
        compiled = run(compile_cmd)
        record: dict = {
            "case": case["name"], "shader": str(shader),
            "types": {k: case[k] for k in ("a", "b", "c", "r")},
            "shape": {"m": 16, "n": 16, "k": 16},
            "compile_returncode": compiled.returncode,
            "compile_stdout": compiled.stdout[-4000:],
            "compile_stderr": compiled.stderr[-4000:],
            "compiler_status": "failed",
            "device_property_status": "not-run",
            "driver_status": "not-run",
            "isa_observed": False,
            "native_matrix_isa_observed": False,
        }
        results.append(record)
        if compiled.returncode != 0 or not spv.is_file() or spv.stat().st_size == 0:
            failures += 1
            print(f"[{stem}] compiler failed: {compiled.stderr[-2000:]}")
            continue
        try:
            opcodes, capability = read_opcodes(spv)
            if not capability or not {
                OP_COOPERATIVE_MATRIX_TYPE_KHR, OP_COOPERATIVE_MATRIX_MUL_ADD_KHR
            }.issubset(opcodes):
                raise ValueError("missing required KHR cooperative-matrix operations")
        except (ValueError, OSError) as exc:
            failures += 1
            record["compile_stderr"] += "\n" + str(exc)
            print(f"[{stem}] invalid compiler output: {exc}")
            continue

        if validator:
            validated = run([validator, "--target-env", "vulkan1.3", str(spv)])
            record["validation_status"] = (
                "pass" if validated.returncode == 0 else "failed"
            )
            record["validation_output"] = (
                validated.stdout + "\n" + validated.stderr
            )[-4000:]
            if validated.returncode != 0:
                failures += 1
                print(f"[{stem}] SPIR-V validation failed")
                continue

        record["compiler_status"] = "pass"
        record["spv"] = str(spv)
        print(f"[{stem}] compiled OpCooperativeMatrixMulAddKHR")

        if not matrix_probe:
            continue

        common_args = [
            "--a-type", case["a"], "--b-type", case["b"],
            "--c-type", case["c"], "--result-type", case["r"],
            "--m", "16", "--n", "16", "--k", "16",
        ]
        properties = run([matrix_probe, "--properties-only", *common_args])
        property_status = classify_properties(properties)
        record["device_property_status"] = property_status
        record["property_returncode"] = properties.returncode
        record["property_output"] = (
            properties.stdout + "\n" + properties.stderr
        )[-4000:]
        if property_status == "unsupported":
            unsupported += 1
            print(f"[{stem}] device does not expose the requested property")
            continue
        if property_status != "supported":
            failures += 1
            print(f"[{stem}] device property query failed")
            continue

        probe = run([matrix_probe, str(spv), str(isa), *common_args])
        record["driver_returncode"] = probe.returncode
        record["driver_output"] = (
            probe.stdout + "\n" + probe.stderr
        )[-4000:]
        status = classify_driver(probe, isa)
        record["driver_status"] = status
        if status == "unsupported":
            unsupported += 1
            print(f"[{stem}] driver ISA extraction unavailable")
            continue
        if status != "completed":
            failures += 1
            print(f"[{stem}] driver pipeline or ISA extraction failed")
            continue

        text = isa.read_text(encoding="utf-8", errors="replace")
        record["isa_observed"] = True
        record["native_matrix_isa_observed"] = bool(
            NATIVE_MATRIX_INSTRUCTION.search(text)
        )
        record["isa"] = str(isa)
        print(f"[{stem}] driver ISA extracted; "
              f"matrix mnemonic observed={record['native_matrix_isa_observed']}")

    overall_status = (
        "failed" if failures else
        "completed-with-unsupported" if unsupported else
        "isa-only" if matrix_probe is None else
        "completed"
    )
    report = {
        "schema": 2,
        "shader": str(shader),
        "overall_status": overall_status,
        "case_count": len(cases),
        "failure_count": failures,
        "unsupported_count": unsupported,
        "cases": results,
        "note": (
            "Compiler coverage establishes legal KHR cooperative-matrix SPIR-V. "
            "Driver pipeline creation and disassembly establish ISA observations, "
            "not execution, numerical equivalence, or performance. Unavailable "
            "device properties or disassembly are not native ISA misses."
        ),
    }
    (out_dir / "khr_matrix_report.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
