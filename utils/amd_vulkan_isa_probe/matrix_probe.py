#!/usr/bin/env python3
"""Query cooperative-matrix capabilities without mistaking compiler blockers for ISA proof."""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path


CASES = (
    {"mode": 0, "name": "f16xf16-f32", "a": "f16", "b": "f16", "c": "f32", "r": "f32",
 "extra": []},
    {"mode": 1, "name": "f16xf16-f16", "a": "f16", "b": "f16", "c": "f16", "r": "f16",
 "extra": []},
    {"mode": 2, "name": "bf16xbf16-f32", "a": "bf16", "b": "bf16", "c": "f32", "r": "f32",
 "extra": ["-fspv-extension=SPV_KHR_bfloat16"]},
    {"mode": 3, "name": "bf16xbf16-bf16", "a": "bf16", "b": "bf16", "c": "bf16", "r": "bf16",
 "extra": ["-fspv-extension=SPV_KHR_bfloat16"]},
    {"mode": 4, "name": "i8xi8-i32", "a": "i8", "b": "i8", "c": "i32", "r": "i32",
 "extra": []},
    {"mode": 5, "name": "u8xu8-u32", "a": "u8", "b": "u8", "c": "u32", "r": "u32",
 "extra": []},
    {"mode": 6, "name": "i8xu8-i32", "a": "i8", "b": "u8", "c": "i32", "r": "i32",
 "extra": []},
    {"mode": 7, "name": "u8xi8-i32", "a": "u8", "b": "i8", "c": "i32", "r": "i32",
 "extra": []},
)

def run(argv: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        argv, text=True, encoding="utf-8", errors="replace",
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )


POINTER_BRIDGE_DIAGNOSTIC = (
    "dx::linalg ByteAddressBuffer cooperative-matrix load/store needs "
    "a typed or untyped pointer bridge at the byte offset"
)


def classify_property(proc: subprocess.CompletedProcess[str]) -> str:
    output = proc.stdout + "\n" + proc.stderr
    for prefix, classification in (
        ("UNSUPPORTED_PROPERTY:", "property-unsupported"),
        ("UNSUPPORTED_FEATURE:", "feature-unsupported"),
        ("UNSUPPORTED:", "extension-or-sdk-unsupported"),
    ):
        if prefix in output:
            return classification
    if (
        proc.returncode == 0
        and "PROPERTY_SUPPORTED" in output
        and "ISA_NOT_TESTED:" in output
    ):
        return "property-supported-isa-not-tested"
    return f"property-probe-error:{proc.returncode}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxc", required=True)
    parser.add_argument("--matrix-probe", required=True)
    parser.add_argument("--out-dir", default="out/amd-matrix-probe")
    parser.add_argument("--target-env", default="vulkan1.3")
    args = parser.parse_args()

    dxc = str(Path(args.dxc).resolve())
    matrix_probe = str(Path(args.matrix_probe).resolve())
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    shader = (
        Path(__file__).resolve().parents[2]
        / "tools/clang/test/CodeGenSPIRV/linalg.cooperative-matrix-shapes.amd.hlsl"
    )

    results: list[dict[str, object]] = []
    hard_failure = False

    for case in CASES:
        name = str(case["name"])
        mode = int(case["mode"])
        compiler = run([
            dxc, "-spirv", "-T", "cs_6_10", "-E", "main",
            f"-fspv-target-env={args.target_env}", *case["extra"],
            f"-DMODE={mode}", str(shader),
        ])
        compiler_output = compiler.stdout + "\n" + compiler.stderr
        expected_blocker = (
            compiler.returncode != 0
            and POINTER_BRIDGE_DIAGNOSTIC in compiler_output
        )
        if not expected_blocker:
            hard_failure = True
            print(
                f"[{name}] unexpected compiler result for matrix "
                "pointer-bridge contract"
            )
            print(compiler_output[-4000:])

        properties = run([
            matrix_probe, "--properties-only",
            "--a-type", str(case["a"]),
            "--b-type", str(case["b"]),
            "--c-type", str(case["c"]),
            "--result-type", str(case["r"]),
            "--m", "16", "--n", "16", "--k", "16",
        ])
        qualification = classify_property(properties)
        if qualification.startswith("property-probe-error"):
            hard_failure = True
        print(
            f"[{name}] compiler-pointer-bridge="
            f"{'blocked-as-expected' if expected_blocker else 'unexpected'}; "
            f"device={qualification}; ISA=not-tested"
        )

        results.append({
            "case": name,
            "mode": mode,
            "types": {
                "a": case["a"], "b": case["b"],
                "c": case["c"], "result": case["r"],
            },
            "shape": {"m": 16, "n": 16, "k": 16},
            "compiler_returncode": compiler.returncode,
            "compiler_pointer_bridge": (
                "blocked-as-expected" if expected_blocker else "unexpected"
            ),
            "compiler_diagnostic": compiler_output.strip()[-4000:],
            "device_property_status": qualification,
            "property_probe_returncode": properties.returncode,
            "property_probe_stdout": properties.stdout,
            "property_probe_stderr": properties.stderr,
            "isa_qualification": "not-tested",
            "isa": None,
        })

    report = {
        "schema": 2,
        "shader": str(shader),
        "cases": results,
        "note": (
            "Compiler descriptor-backed matrix operations currently require "
            "an unimplemented legal pointer bridge. Device cooperative-matrix "
            "properties are queried independently. Property support is not "
            "proof of native WMMA/SWMMAC/MFMA lowering; matrix ISA is untested."
        ),
    }
    (out_dir / "matrix_report.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    return 5 if hard_failure else 0


if __name__ == "__main__":
    raise SystemExit(main())
