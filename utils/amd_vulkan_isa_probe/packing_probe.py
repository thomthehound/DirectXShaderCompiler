#!/usr/bin/env python3
"""Compile packed-conversion candidates and classify Radeon native recovery."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path


EXPECTED = {
    "pknorm_i16_f32": re.compile(r"\bv_cvt_pknorm_i16_f32\b", re.I),
    "pknorm_u16_f32": re.compile(r"\bv_cvt_pknorm_u16_f32\b", re.I),
    "pk_i16_i32": re.compile(r"\bv_cvt_pk_i16_i32\b", re.I),
    "pk_u16_u32": re.compile(r"\bv_cvt_pk_u16_u32\b", re.I),
    "pk_u8_f32": re.compile(r"\bv_cvt_pk_u8_f32\b", re.I),
}


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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxc", required=True)
    parser.add_argument("--driver-probe", required=True)
    parser.add_argument("--out-dir", default="out/amd-packing-probe")
    parser.add_argument("--target-env", default="vulkan1.2")
    args = parser.parse_args()

    dxc = str(Path(args.dxc).resolve())
    driver_probe = str(Path(args.driver_probe).resolve())
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    shader = Path(__file__).resolve().parent / "shaders/packing_native.hlsl"
    spv = out_dir / "packing_native.spv"
    isa = out_dir / "packing_native.driver.isa"

    compiled = run([
        dxc, "-spirv", "-T", "cs_6_2", "-E", "main",
        f"-fspv-target-env={args.target_env}", str(shader), "-Fo", str(spv),
    ])
    if compiled.returncode != 0:
        print(compiled.stdout, end="")
        print(compiled.stderr, end="")
        return compiled.returncode or 2

    probed = run([driver_probe, str(spv), str(isa)])
    print(probed.stdout, end="")
    print(probed.stderr, end="")
    if probed.returncode != 0:
        return probed.returncode
    if not isa.exists():
        print("driver probe succeeded but produced no ISA file")
        return 3

    text = isa.read_text(encoding="utf-8", errors="replace")
    counts = {name: len(pattern.findall(text)) for name, pattern in EXPECTED.items()}
    report = {
        "shader": str(shader),
        "spv": str(spv),
        "isa": str(isa),
        "native_conversion_counts": counts,
        "note": (
            "PackSnorm2x16/PackUnorm2x16 are direct GLSL.std.450 contracts. "
            "PackI16x2/PackU16x2 are exact clamp-and-pack semantic graphs. "
            "PackUnorm4x8 is only a higher-level candidate for pk_u8 recovery; "
            "absence or presence of v_cvt_pk_u8_f32 is not promoted to exact "
            "intrinsic parity without separate rounding/clamp qualification."
        ),
    }
    (out_dir / "packing_report.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )

    width = max(len(name) for name in counts)
    for name, count in counts.items():
        print(f"{name:<{width}} : {count}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
