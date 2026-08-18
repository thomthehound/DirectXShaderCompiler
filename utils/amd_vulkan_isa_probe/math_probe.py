#!/usr/bin/env python3
"""Compile AMD-oriented math probes and report native Radeon ISA recovery."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path


EXPECTED = {
    "bfe_u32": re.compile(r"\bv_bfe_u32\b", re.I),
    "bfe_i32": re.compile(r"\bv_bfe_i32\b", re.I),
    "mul_u32_u24": re.compile(r"\bv_mul_u32_u24\b", re.I),
    "mul_i32_i24": re.compile(r"\bv_mul_i32_i24\b", re.I),
    "mul_hi_u32_u24": re.compile(r"\bv_mul_hi_u32_u24\b", re.I),
    "mul_hi_i32_i24": re.compile(r"\bv_mul_hi_i32_i24\b", re.I),
    "mad_u32_u24": re.compile(r"\bv_mad_u32_u24\b", re.I),
    "mad_i32_i24": re.compile(r"\bv_mad_i32_i24\b", re.I),
    "lerp_u8": re.compile(r"\bv_lerp_u8\b", re.I),
    "bfi_b32": re.compile(r"\bv_bfi_b32\b", re.I),
    "xad_u32": re.compile(r"\bv_xad_u32\b", re.I),
    "lshl_add_u32": re.compile(r"\bv_lshl_add_u32\b", re.I),
    "add_lshl_u32": re.compile(r"\bv_add_lshl_u32\b", re.I),
    "add3_u32": re.compile(r"\bv_add3_u32\b", re.I),
    "lshl_or_b32": re.compile(r"\bv_lshl_or_b32\b", re.I),
    "and_or_b32": re.compile(r"\bv_and_or_b32\b", re.I),
    "or3_b32": re.compile(r"\bv_or3_b32\b", re.I),
    "sad_u8": re.compile(r"\bv_sad_u8\b", re.I),
    "sad_hi_u8": re.compile(r"\bv_sad_hi_u8\b", re.I),
    "sad_u16": re.compile(r"\bv_sad_u16\b", re.I),
    "sad_u32": re.compile(r"\bv_sad_u32\b", re.I),
    "msad_u8": re.compile(r"\bv_msad_u8\b", re.I),
    "rcp_f32": re.compile(r"\bv_rcp_f32\b", re.I),
    "sqrt_f32": re.compile(r"\bv_sqrt_f32\b", re.I),
    "rsq_f32": re.compile(r"\bv_rsq_f32\b", re.I),
    "sin_f32": re.compile(r"\bv_sin_f32\b", re.I),
    "cos_f32": re.compile(r"\bv_cos_f32\b", re.I),
    "log_f32": re.compile(r"\bv_log_f32\b", re.I),
    "exp_f32": re.compile(r"\bv_exp_f32\b", re.I),
    "med3_f32": re.compile(r"\bv_med3_f32\b", re.I),
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
    parser.add_argument("--out-dir", default="out/amd-math-native-probe")
    parser.add_argument("--target-env", default="vulkan1.2")
    args = parser.parse_args()

    dxc = str(Path(args.dxc).resolve())
    driver_probe = str(Path(args.driver_probe).resolve())
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    shader = Path(__file__).resolve().parent / "shaders" / "math_core_native.hlsl"
    spv = out_dir / "math_core_native.spv"
    isa = out_dir / "math_core_native.driver.isa"

    compile_argv = [
        dxc, "-spirv", "-T", "cs_6_2", "-E", "main",
        f"-fspv-target-env={args.target_env}", "-fspv-extension=AMD",
        str(shader), "-Fo", str(spv),
    ]
    compiled = run(compile_argv)
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
        "all_expected_recovered": all(results.values()),
    }
    (out_dir / "math_report.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )

    width = max(len(name) for name in results)
    for name, recovered in results.items():
        print(f"{name:<{width}} : {'NATIVE' if recovered else 'not found'}")

    # Missing a mnemonic is evidence, not a harness failure. Keep exit 0 so one
    # driver optimization miss does not hide the rest of the report.
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
