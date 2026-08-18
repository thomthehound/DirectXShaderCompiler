#!/usr/bin/env python3
"""Run the AMD-oriented DXIL contract regression with only a built dxc."""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path


REQUIRED = (
    "@dx.op.dot4AddPacked.i32(i32 163,",
    "@dx.op.dot4AddPacked.i32(i32 164,",
    "dx.op.waveReadLaneAt",
    "dx.op.waveActiveOp",
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxc", required=True)
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    shader = root / "tools/clang/test/HLSLFileCheck/hlsl/amd/dxil-parity.hlsl"
    dxc = str(Path(args.dxc).resolve())

    with tempfile.TemporaryDirectory(prefix="amd_dxil_ci_") as temp_name:
        temp = Path(temp_name)
        blob = temp / "dxil-parity.dxil"
        listing = temp / "dxil-parity.ll"
        proc = subprocess.run(
            [dxc, "-E", "main", "-T", "cs_6_4", str(shader),
             "-Fo", str(blob), "-Fc", str(listing)],
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
        if not listing.is_file():
            print("DXC succeeded but did not produce a DXIL listing", file=sys.stderr)
            return 3
        text = listing.read_text(encoding="utf-8", errors="replace")
        missing = [needle for needle in REQUIRED if needle not in text]
        if missing:
            print("AMD DXIL contract regression failed; missing:", file=sys.stderr)
            for needle in missing:
                print(f"  {needle}", file=sys.stderr)
            return 4

    print("AMD DXIL backend contracts: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
