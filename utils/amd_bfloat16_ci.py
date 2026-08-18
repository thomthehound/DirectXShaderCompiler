#!/usr/bin/env python3
"""Compile and verify first-class SPV_KHR_bfloat16 conversion contracts."""

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
    shader = root / "tools/clang/test/CodeGenSPIRV/amd.bfloat16.conversion.hlsl"
    dxc = str(Path(args.dxc).resolve())
    proc = subprocess.run(
        [
            dxc,
            "-T", "cs_6_2",
            "-E", "main",
            "-enable-16bit-types",
            "-fcgl",
            "-spirv",
            "-fspv-target-env=vulkan1.3",
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
        print(proc.stdout, end="")
        print(proc.stderr, end="", file=sys.stderr)
        return proc.returncode or 2

    required = (
        r"\bOpCapability BFloat16TypeKHR\b",
        r'OpExtension "SPV_KHR_bfloat16"',
        r"OpTypeFloat 16 BFloat16KHR",
        r"\bOpTypeVector\b.*\b2\b",
    )
    missing = [pattern for pattern in required if re.search(pattern, proc.stdout) is None]
    convert_count = len(re.findall(r"\bOpFConvert\b", proc.stdout))
    bitcast_count = len(re.findall(r"\bOpBitcast\b", proc.stdout))
    if missing or convert_count != 4 or bitcast_count < 4:
        print("BF16 conversion contract regression failed", file=sys.stderr)
        for pattern in missing:
            print(f"  missing: {pattern}", file=sys.stderr)
        print(f"  OpFConvert count: {convert_count} (expected 4)", file=sys.stderr)
        print(f"  OpBitcast count: {bitcast_count} (expected at least 4)", file=sys.stderr)
        return 3

    print("SPV_KHR BF16 scalar/vector conversion contracts: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
