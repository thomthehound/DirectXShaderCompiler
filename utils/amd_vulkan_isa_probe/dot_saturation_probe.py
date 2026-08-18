#!/usr/bin/env python3
"""Compare direct SPIR-V saturating dot with canonical widened-clamp graphs."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path

SCENARIOS = (
    ("direct", "dot_saturation_direct.hlsl"),
    ("canonical", "dot_saturation_canonical.hlsl"),
)
DOT_LINE = re.compile(r".*\bv_dot[0-9A-Za-z_]*.*", re.I)


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
    parser.add_argument("--out-dir", default="out/amd-dot-saturation-probe")
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
        spv = out_dir / f"dot_sat_{name}.spv"
        isa = out_dir / f"dot_sat_{name}.driver.isa"
        compiled = run([
            dxc, "-spirv", "-T", "cs_6_4", "-E", "main",
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
        dot_lines = [line.strip() for line in text.splitlines() if DOT_LINE.match(line)]
        result = {
            "shader": str(shader),
            "spv": str(spv),
            "isa": str(isa),
            "dot_lines": dot_lines,
            "dot_instruction_count": len(dot_lines),
            "clamp_marked_dot_count": sum("clamp" in line.lower() for line in dot_lines),
        }
        scenarios[name] = result
        print(
            f"[{name}] dot={result['dot_instruction_count']} "
            f"dot+clamp={result['clamp_marked_dot_count']}"
        )
        for line in dot_lines:
            print(f"  {line}")

    report = {
        "scenarios": scenarios,
        "interpretation": (
            "Direct uses SPV_KHR_integer_dot_product *DotAccSat instructions. Canonical "
            "uses independent lane extraction, 64-bit accumulation and explicit clamp. "
            "A native dot hit is codegen evidence only; runtime boundary vectors must prove "
            "saturation equivalence before a canonical form is promoted."
        ),
    }
    (out_dir / "dot_saturation_report.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
