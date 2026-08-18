#!/usr/bin/env python3
"""Compile AMD packed-dot candidates and report native Radeon ISA recovery."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path


EXPECTED = {
    "dot4_i8": re.compile(r"\bv_dot4(?:c)?_i32_i8\b", re.I),
    "dot4_u8": re.compile(r"\bv_dot4(?:c)?_u32_u8\b", re.I),
    "dot2_i16": re.compile(r"\bv_dot2(?:c)?_i32_i16\b", re.I),
    "dot2_u16": re.compile(r"\bv_dot2(?:c)?_u32_u16\b", re.I),
    "dot4_mixed_i8_u8": re.compile(r"\bv_dot4_i32_iu8\b", re.I),
    "dot8_i4": re.compile(r"\bv_dot8(?:c)?_i32_i4\b", re.I),
    "dot8_u4": re.compile(r"\bv_dot8_u32_u4\b", re.I),
    "dot8_mixed_i4_u4": re.compile(r"\bv_dot8_i32_iu4\b", re.I),
    "dot2_f32_f16_bits": re.compile(r"\bv_dot2(?:acc|c)?_f32_f16(?:_e32)?\b", re.I),
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
    parser.add_argument("--out-dir", default="out/amd-dot-native-probe")
    parser.add_argument("--target-env", default="vulkan1.2")
    args = parser.parse_args()

    dxc = str(Path(args.dxc).resolve())
    driver_probe = str(Path(args.driver_probe).resolve())
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    shader = Path(__file__).resolve().parent / "shaders" / "dot_native.hlsl"
    spv = out_dir / "dot_native.spv"
    isa = out_dir / "dot_native.driver.isa"

    compiled = run([
        dxc,
        "-spirv",
        "-T", "cs_6_4",
        "-E", "main",
        f"-fspv-target-env={args.target_env}",
        str(shader),
        "-Fo", str(spv),
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
    results = {name: bool(pattern.search(text)) for name, pattern in EXPECTED.items()}
    report = {
        "shader": str(shader),
        "spv": str(spv),
        "isa": str(isa),
        "native_recovery": results,
        "note": (
            "Instruction availability is GPU-generation dependent. Missing i4, mixed-sign, "
            "legacy integer-dot, or packed-half FP16-dot variants is evidence about this "
            "driver/GPU, not a harness failure. The packed-half FP16 candidate deliberately "
            "uses uint input bits plus f16tof32 so the device does not need shaderFloat16 "
            "enabled merely to test v_dot2_f32_f16 recovery."
        ),
    }
    (out_dir / "dot_report.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )

    width = max(len(name) for name in results)
    for name, recovered in results.items():
        print(f"{name:<{width}} : {'NATIVE' if recovered else 'not found'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
