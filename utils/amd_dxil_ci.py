#!/usr/bin/env python3
"""Run AMD-oriented DXIL contract regressions with only a built dxc."""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path


def compile_listing(
    dxc: str,
    shader: Path,
    target: str,
    required: tuple[str, ...],
    temp: Path,
    stem: str,
) -> int:
    blob = temp / f"{stem}.dxil"
    listing = temp / f"{stem}.ll"
    proc = subprocess.run(
        [dxc, "-E", "main", "-T", target, str(shader),
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
        print(f"{stem}: DXC succeeded but produced no DXIL listing", file=sys.stderr)
        return 3
    text = listing.read_text(encoding="utf-8", errors="replace")
    missing = [needle for needle in required if needle not in text]
    if missing:
        print(f"{stem}: AMD DXIL contract regression failed; missing:", file=sys.stderr)
        for needle in missing:
            print(f"  {needle}", file=sys.stderr)
        return 4
    print(f"PASS AMD DXIL: {stem}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxc", required=True)
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    tests = root / "tools/clang/test/HLSLFileCheck/hlsl/amd"
    dxc = str(Path(args.dxc).resolve())

    i64_atomic = ("dx.op.atomicBinOp.i64", "dx.op.atomicCompareExchange.i64")
    cases = (
        (
            "core-parity",
            tests / "dxil-parity.hlsl",
            "cs_6_4",
            (
                "@dx.op.dot4AddPacked.i32(i32 163,",
                "@dx.op.dot4AddPacked.i32(i32 164,",
                "dx.op.waveReadLaneAt",
                "dx.op.waveActiveOp",
            ),
        ),
        (
            "draw-parameters",
            tests / "dxil-draw-parameters.hlsl",
            "vs_6_8",
            ("dx.op.startVertexLocation", "dx.op.startInstanceLocation"),
        ),
        ("atomic-u64-structured", tests / "dxil-atomic-u64.hlsl", "cs_6_6", i64_atomic),
        ("atomic-u64-byteaddress", tests / "dxil-atomic-u64-byteaddress.hlsl", "cs_6_6", i64_atomic),
        ("atomic-u64-image-ops", tests / "dxil-atomic-u64-image.hlsl", "cs_6_6", i64_atomic),
        ("atomic-u64-image-shapes", tests / "dxil-atomic-u64-image-shapes.hlsl", "cs_6_6", i64_atomic),
    )

    with tempfile.TemporaryDirectory(prefix="amd_dxil_ci_") as temp_name:
        temp = Path(temp_name)
        for stem, shader, target, required in cases:
            result = compile_listing(dxc, shader, target, required, temp, stem)
            if result != 0:
                return result

    print("AMD DXIL backend contracts: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
