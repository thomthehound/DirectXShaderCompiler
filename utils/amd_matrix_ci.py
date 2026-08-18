#!/usr/bin/env python3
"""Compile the APUSR-relevant AMD cooperative-matrix shape/type corpus."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path


CASES = (
    (0, "f16xf16-f32", ()),
    (1, "f16xf16-f16", ()),
    (2, "bf16xbf16-f32", ("-fspv-extension=SPV_KHR_bfloat16",)),
    (3, "bf16xbf16-bf16", ("-fspv-extension=SPV_KHR_bfloat16",)),
    (4, "i8xi8-i32", ()),
    (5, "u8xu8-u32", ()),
    (6, "i8xu8-i32", ()),
    (7, "u8xi8-i32", ()),
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxc", required=True)
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    shader = root / "tools/clang/test/CodeGenSPIRV/linalg.cooperative-matrix-shapes.amd.hlsl"
    dxc = str(Path(args.dxc).resolve())

    required = (
        r"OpCapability CooperativeMatrixKHR",
        r'OpExtension "SPV_KHR_cooperative_matrix"',
        r"\bOpTypeCooperativeMatrixKHR\b",
        r"\bOpCooperativeMatrixLoadKHR\b",
        r"\bOpCooperativeMatrixMulAddKHR\b",
        r"\bOpCooperativeMatrixStoreKHR\b",
    )

    for mode, label, extra in CASES:
        proc = subprocess.run(
            [
                dxc, "-T", "cs_6_10", "-E", "main", "-fcgl", "-spirv",
                "-fspv-target-env=vulkan1.3", *extra, f"-DMODE={mode}",
                str(shader),
            ],
            text=True,
            encoding="utf-8",
            errors="replace",
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if proc.returncode != 0:
            print(f"AMD matrix {label}: compilation failed", file=sys.stderr)
            print(proc.stdout, end="")
            print(proc.stderr, end="", file=sys.stderr)
            return proc.returncode or 2
        missing = [pattern for pattern in required if re.search(pattern, proc.stdout) is None]
        if mode in (2, 3) and 'OpExtension "SPV_KHR_bfloat16"' not in proc.stdout:
            missing.append('OpExtension "SPV_KHR_bfloat16"')
        if missing:
            print(f"AMD matrix {label}: missing SPIR-V contracts:", file=sys.stderr)
            for pattern in missing:
                print(f"  {pattern}", file=sys.stderr)
            return 3
        print(f"PASS AMD matrix: {label}")

    print("AMD cooperative matrix shape contracts: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
