#!/usr/bin/env python3
"""Run AMD packed-conversion SPIR-V contract regressions with a built dxc."""

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
    shader = root / "tools/clang/test/CodeGenSPIRV/amd.math.packing.hlsl"
    dxc = str(Path(args.dxc).resolve())
    proc = subprocess.run(
        [
            dxc, "-T", "cs_6_2", "-E", "main", "-fcgl", "-spirv",
            "-fspv-target-env=vulkan1.1", str(shader),
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
        r'OpExtInstImport "GLSL.std.450"',
        r"\bOpExtInst\b.*\bPackSnorm2x16\b",
        r"\bOpExtInst\b.*\bPackUnorm2x16\b",
        r"\bOpExtInst\b.*\bPackSnorm4x8\b",
        r"\bOpExtInst\b.*\bPackUnorm4x8\b",
        r"\bOpBitwiseAnd\b",
        r"\bOpShiftLeftLogical\b",
        r"\bOpBitwiseOr\b",
    )
    missing = [pattern for pattern in required if re.search(pattern, proc.stdout) is None]
    if missing:
        print("AMD packing contract regression failed; missing:", file=sys.stderr)
        for pattern in missing:
            print(f"  {pattern}", file=sys.stderr)
        return 3

    print("AMD packed conversion contracts: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
