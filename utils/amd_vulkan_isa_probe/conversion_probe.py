#!/usr/bin/env python3
"""Compare scalar and packed FP16 conversion spellings in Radeon ISA."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path

SCENARIOS = (
    ("scalar", "half_convert_scalar.hlsl"),
    ("packed", "half_convert_packed.hlsl"),
)
EXPECTED = {
    "cvt_f16_f32": re.compile(r"\bv_cvt_f16_f32\b", re.I),
    "cvt_f32_f16": re.compile(r"\bv_cvt_f32_f16\b", re.I),
    "cvt_pkrtz_f16_f32": re.compile(r"\bv_cvt_pkrtz_f16_f32\b", re.I),
    "pack_b32_f16": re.compile(r"\bv_pack_b32_f16\b", re.I),
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
    parser.add_argument("--out-dir", default="out/amd-half-conversion-probe")
    parser.add_argument("--target-env", default="vulkan1.2")
    args = parser.parse_args()

    dxc = str(Path(args.dxc).resolve())
    driver_probe = str(Path(args.driver_probe).resolve())
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    shader_root = Path(__file__).resolve().parent / "shaders"

    scenarios: dict[str, object] = {}
    for name, shader_name in SCENARIOS:
        shader = shader_root / shader_name
        spv = out_dir / f"half_{name}.spv"
        isa = out_dir / f"half_{name}.driver.isa"
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
            print(f"{name}: driver probe succeeded but produced no ISA file")
            return 3

        text = isa.read_text(encoding="utf-8", errors="replace")
        counts = {key: len(pattern.findall(text)) for key, pattern in EXPECTED.items()}
        scenarios[name] = {"shader": str(shader), "spv": str(spv), "isa": str(isa), "counts": counts}
        print(f"[{name}] {counts}")

    report = {
        "scenarios": scenarios,
        "interpretation": (
            "Both scenarios use standard HLSL FP32<->FP16 semantics. The packed path keeps "
            "two adjacent values together so the backend can select packed conversion/pack "
            "instructions when profitable. Numerical boundary and tie-rounding tests remain "
            "the qualification oracle."
        ),
    }
    (out_dir / "half_conversion_report.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
