#!/usr/bin/env python3
"""Compile APUSR-relevant cooperative-matrix shapes through SPIR-V and DXIL."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path


CASES = (
    {"mode": 0, "name": "f16xf16-f32", "a": 8, "b": 8, "c": 9, "spv_extra": ()},
    {"mode": 1, "name": "f16xf16-f16", "a": 8, "b": 8, "c": 8, "spv_extra": ()},
    {"mode": 2, "name": "bf16xbf16-f32", "a": 23, "b": 23, "c": 9,
     "spv_extra": ("-fspv-extension=SPV_KHR_bfloat16",)},
    {"mode": 3, "name": "bf16xbf16-bf16", "a": 23, "b": 23, "c": 23,
     "spv_extra": ("-fspv-extension=SPV_KHR_bfloat16",)},
    {"mode": 4, "name": "i8xi8-i32", "a": 19, "b": 19, "c": 4, "spv_extra": ()},
    {"mode": 5, "name": "u8xu8-u32", "a": 20, "b": 20, "c": 5, "spv_extra": ()},
    {"mode": 6, "name": "i8xu8-i32", "a": 19, "b": 20, "c": 4, "spv_extra": ()},
    {"mode": 7, "name": "u8xi8-i32", "a": 20, "b": 19, "c": 4, "spv_extra": ()},
)


def run(argv: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        argv,
        text=True,
        encoding="utf-8",
        errors="replace",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def fail(label: str, backend: str, proc: subprocess.CompletedProcess[str], reason: str) -> int:
    print(f"AMD matrix {label} / {backend}: {reason}", file=sys.stderr)
    print(proc.stdout, end="")
    print(proc.stderr, end="", file=sys.stderr)
    return proc.returncode or 2


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxc", required=True)
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    shader = root / "tools/clang/test/CodeGenSPIRV/linalg.cooperative-matrix-shapes.amd.hlsl"
    dxc = str(Path(args.dxc).resolve())

    spv_required = (
        r"OpCapability CooperativeMatrixKHR",
        r'OpExtension "SPV_KHR_cooperative_matrix"',
        r"\bOpTypeCooperativeMatrixKHR\b",
        r"\bOpCooperativeMatrixLoadKHR\b",
        r"\bOpCooperativeMatrixMulAddKHR\b",
        r"\bOpCooperativeMatrixStoreKHR\b",
    )

    with tempfile.TemporaryDirectory(prefix="amd_matrix_ci_") as temp_name:
        temp = Path(temp_name)
        for case in CASES:
            label = str(case["name"])
            mode = int(case["mode"])
            spv_proc = run([
                dxc, "-T", "cs_6_10", "-E", "main", "-fcgl", "-spirv",
                "-fspv-target-env=vulkan1.3", *case["spv_extra"],
                f"-DMODE={mode}", str(shader),
            ])
            if spv_proc.returncode != 0:
                return fail(label, "SPIR-V", spv_proc, "compilation failed")
            missing = [p for p in spv_required if re.search(p, spv_proc.stdout) is None]
            if mode in (2, 3) and 'OpExtension "SPV_KHR_bfloat16"' not in spv_proc.stdout:
                missing.append('OpExtension "SPV_KHR_bfloat16"')
            if missing:
                print(f"AMD matrix {label} / SPIR-V missing contracts:", file=sys.stderr)
                for pattern in missing:
                    print(f"  {pattern}", file=sys.stderr)
                return 3
            print(f"PASS AMD matrix SPIR-V: {label}")

            dxil = temp / f"{label}.dxil"
            listing = temp / f"{label}.ll"
            dxil_proc = run([
                dxc, "-T", "cs_6_10", "-E", "main", f"-DMODE={mode}",
                str(shader), "-Fo", str(dxil), "-Fc", str(listing),
            ])
            if dxil_proc.returncode != 0:
                return fail(label, "DXIL", dxil_proc, "compilation failed")
            if not listing.is_file():
                print(f"AMD matrix {label} / DXIL produced no listing", file=sys.stderr)
                return 4
            text = listing.read_text(encoding="utf-8", errors="replace")
            dxil_required = (
                "dx.op.linAlgMatrixMultiplyAccumulate",
                f"LinAlgMatrixC{case['a']}M16N16U0S1",
                f"LinAlgMatrixC{case['b']}M16N16U1S1",
                f"LinAlgMatrixC{case['c']}M16N16U2S1",
            )
            missing_dxil = [needle for needle in dxil_required if needle not in text]
            if missing_dxil:
                print(f"AMD matrix {label} / DXIL missing contracts:", file=sys.stderr)
                for needle in missing_dxil:
                    print(f"  {needle}", file=sys.stderr)
                return 5
            print(f"PASS AMD matrix DXIL: {label}")

    print("AMD cooperative matrix SPIR-V + DXIL shape contracts: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
