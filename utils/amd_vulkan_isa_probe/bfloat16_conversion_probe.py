#!/usr/bin/env python3
"""Classify Radeon lowering of first-class SPIR-V BF16 conversions."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path


CASES = (
    (0, "f32x2-to-bf16x2"),
    (1, "bf16x2-to-f32x2"),
)

PATTERNS = {
    "cvt_pk_bf16_f32": re.compile(r"\bv_cvt_pk_bf16_f32\b", re.I),
    "cvt_sr_pk_bf16_f32": re.compile(r"\bv_cvt_sr_pk_bf16_f32\b", re.I),
    "shift_left_16": re.compile(r"\bv_lshl(?:rev)?_b32\b[^\n]*\b16\b", re.I),
    "shift_right_16": re.compile(r"\bv_lshr(?:rev)?_b32\b[^\n]*\b16\b", re.I),
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
    parser.add_argument("--out-dir", default="out/amd-bfloat16-conversion-probe")
    parser.add_argument("--target-env", default="vulkan1.3")
    args = parser.parse_args()

    dxc = str(Path(args.dxc).resolve())
    driver_probe = str(Path(args.driver_probe).resolve())
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    shader = Path(__file__).resolve().parent / "shaders/bfloat16_conversion_native.hlsl"

    report: dict[str, object] = {
        "shader": str(shader),
        "contract": (
            "SPV_KHR_bfloat16 OpFConvert on genuine BFloat16KHR types. "
            "BF16->F32 is exact; Vulkan does not define one universal bit result "
            "for F32->BF16 narrowing, so ISA recovery is classified rather than "
            "treated as exact rounding-mode proof."
        ),
        "cases": [],
    }

    for mode, label in CASES:
        case_dir = out_dir / label
        case_dir.mkdir(parents=True, exist_ok=True)
        spv = case_dir / "shader.spv"
        isa = case_dir / "driver.isa"

        compiled = run([
            dxc, "-spirv", "-T", "cs_6_2", "-E", "main",
            "-enable-16bit-types", f"-fspv-target-env={args.target_env}",
            f"-DMODE={mode}", str(shader), "-Fo", str(spv),
        ])
        if compiled.returncode != 0:
            print(compiled.stdout, end="")
            print(compiled.stderr, end="")
            return compiled.returncode or 2

        probed = run([
            driver_probe, str(spv), str(isa), "--enable-bfloat16",
            "--push-constant-bytes", "0",
        ])
        print(probed.stdout, end="")
        print(probed.stderr, end="")

        case: dict[str, object] = {
            "label": label,
            "mode": mode,
            "spv": str(spv),
            "probe_returncode": probed.returncode,
        }
        if probed.returncode in (5, 6):
            case["status"] = "unsupported-bfloat16"
            case["detail"] = (probed.stdout + probed.stderr).strip()
            report["cases"].append(case)
            continue
        if probed.returncode != 0:
            return probed.returncode
        if not isa.exists():
            print(f"{label}: driver probe succeeded but produced no ISA file")
            return 3

        text = isa.read_text(encoding="utf-8", errors="replace")
        counts = {name: len(pattern.findall(text)) for name, pattern in PATTERNS.items()}
        case["status"] = "isa-produced"
        case["isa"] = str(isa)
        case["counts"] = counts
        report["cases"].append(case)

        rendered = ", ".join(f"{name}={count}" for name, count in counts.items())
        print(f"{label}: {rendered}")

    (out_dir / "bfloat16_conversion_report.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
