#!/usr/bin/env python3
"""Run exact mixed-precision FP16/BF16 SPIR-V dot contract regressions."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxc", required=True)
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    shader = root / "tools/clang/test/CodeGenSPIRV/amd.mixed-dot.hlsl"
    dxc = str(Path(args.dxc).resolve())
    proc = subprocess.run(
        [
            dxc, "-T", "cs_6_2", "-E", "main", "-enable-16bit-types",
            "-fcgl", "-spirv", "-fspv-target-env=vulkan1.3", str(shader),
        ],
        text=True,
        encoding="utf-8",
        errors="replace",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if proc.returncode != 0:
        print(proc.stdout, end="")
        print(proc.stderr, end="", file=sys.stderr)
        return proc.returncode or 2

    required = (
        r"OpCapability Float16",
        r"OpCapability BFloat16TypeKHR",
        r"OpCapability BFloat16DotProductKHR",
        r"OpCapability DotProductFloat16AccFloat32VALVE",
        r"OpCapability DotProductFloat16AccFloat16VALVE",
        r"OpCapability DotProductBFloat16AccVALVE",
        r'OpExtension "SPV_KHR_bfloat16"',
        r'OpExtension "SPV_VALVE_mixed_float_dot_product"',
        r"OpTypeFloat 16 BFloat16KHR",
        r"\bOpFDot2MixAcc32VALVE\b",
        r"\bOpFDot2MixAcc16VALVE\b",
        r"\bOpDot\b",
    )
    missing = [pattern for pattern in required if re.search(pattern, proc.stdout) is None]
    if missing:
        print("AMD mixed-dot contract regression failed; missing:", file=sys.stderr)
        for pattern in missing:
            print(f"  {pattern}", file=sys.stderr)
        return 3

    if len(re.findall(r"\bOpFDot2MixAcc32VALVE\b", proc.stdout)) != 2:
        print("expected exactly two 32-bit mixed-dot instructions", file=sys.stderr)
        return 4
    if len(re.findall(r"\bOpFDot2MixAcc16VALVE\b", proc.stdout)) != 2:
        print("expected exactly two 16-bit mixed-dot instructions", file=sys.stderr)
        return 5

    print("AMD FP16/BF16 mixed-dot SPIR-V contracts: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
