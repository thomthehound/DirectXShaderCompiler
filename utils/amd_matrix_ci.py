#!/usr/bin/env python3
"""Qualify APUSR-relevant cooperative-matrix shapes through SPIR-V and DXIL."""

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

SPV_BYTEADDRESS_BLOCKER = (
    "dx::linalg ByteAddressBuffer cooperative-matrix load/store needs a typed "
    "or untyped pointer bridge at the byte offset; refusing to reinterpret a "
    "Vulkan storage-buffer pointer illegally"
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


def report_process(label: str, backend: str, proc: subprocess.CompletedProcess[str], reason: str) -> None:
    print(f"AMD matrix {label} / {backend}: {reason}", file=sys.stderr)
    print(proc.stdout, end="")
    print(proc.stderr, end="", file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxc", required=True)
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    shader = root / "tools/clang/test/CodeGenSPIRV/linalg.cooperative-matrix-shapes.amd.hlsl"
    dxc = str(Path(args.dxc).resolve())
    failed = False

    with tempfile.TemporaryDirectory(prefix="amd_matrix_ci_") as temp_name:
        temp = Path(temp_name)
        for case in CASES:
            label = str(case["name"])
            mode = int(case["mode"])

            # This descriptor-backed ByteAddressBuffer path is intentionally not
            # implemented for SPIR-V yet. A legal typed/untyped pointer bridge at
            # the byte offset is required; accepting the current shader would mean
            # we had silently reintroduced the illegal pointer reinterpretation.
            spv_proc = run([
                dxc, "-T", "cs_6_10", "-E", "main", "-fcgl", "-spirv",
                "-fspv-target-env=vulkan1.3", *case["spv_extra"],
                f"-DMODE={mode}", str(shader),
            ])
            spv_output = spv_proc.stdout + "\n" + spv_proc.stderr
            if spv_proc.returncode == 0:
                report_process(
                    label, "SPIR-V", spv_proc,
                    "unexpectedly accepted blocked ByteAddressBuffer pointer bridge",
                )
                failed = True
            elif SPV_BYTEADDRESS_BLOCKER not in spv_output:
                report_process(
                    label, "SPIR-V", spv_proc,
                    "failed for a reason other than the tracked pointer-bridge blocker",
                )
                failed = True
            else:
                print(
                    f"PASS AMD matrix SPIR-V blocker: {label} rejects unsafe "
                    "ByteAddressBuffer pointer reinterpretation"
                )

            dxil = temp / f"{label}.dxil"
            listing = temp / f"{label}.ll"
            dxil_proc = run([
                dxc, "-T", "cs_6_10", "-E", "main", f"-DMODE={mode}",
                str(shader), "-Fo", str(dxil), "-Fc", str(listing),
            ])
            if dxil_proc.returncode != 0:
                report_process(label, "DXIL", dxil_proc, "compilation failed")
                failed = True
                continue
            if not listing.is_file():
                print(
                    f"AMD matrix {label} / DXIL produced no listing",
                    file=sys.stderr,
                )
                failed = True
                continue
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
                failed = True
                continue
            print(f"PASS AMD matrix DXIL: {label}")

    if failed:
        return 1

    print("AMD cooperative matrix DXIL shapes + SPIR-V pointer-bridge blocker: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
