#!/usr/bin/env python3
"""Check Radeon recovery of registered AMD cube SPIR-V operations."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path

CUBE = {
    "cubeid": re.compile(r"\bv_cubeid_f(?:16|32)\b", re.I),
    "cubema": re.compile(r"\bv_cubema_f(?:16|32)\b", re.I),
    "cubesc": re.compile(r"\bv_cubesc_f(?:16|32)\b", re.I),
    "cubetc": re.compile(r"\bv_cubetc_f(?:16|32)\b", re.I),
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
    parser.add_argument("--out-dir", default="out/amd-cube-probe")
    parser.add_argument("--target-env", default="vulkan1.2")
    args = parser.parse_args()

    dxc = str(Path(args.dxc).resolve())
    driver_probe = str(Path(args.driver_probe).resolve())
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    shader = Path(__file__).resolve().parent / "shaders/cube_native.hlsl"
    spv = out_dir / "cube_native.spv"
    isa = out_dir / "cube_native.driver.isa"

    compiled = run([
        dxc, "-spirv", "-T", "cs_6_0", "-E", "main",
        f"-fspv-target-env={args.target_env}", "-fspv-extension=AMD",
        str(shader), "-Fo", str(spv),
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
    counts = {name: len(pattern.findall(text)) for name, pattern in CUBE.items()}
    report = {
        "shader": str(shader),
        "spv": str(spv),
        "isa": str(isa),
        "native_cube_counts": counts,
        "all_four_native_helpers_seen": all(counts.values()),
        "interpretation": (
            "SPV_AMD_gcn_shader preserves face-index/face-coordinate semantics. "
            "The four low-level cube helpers are backend implementation details; this "
            "probe reports whether Radeon recovers them without requiring APUSR to spell "
            "their intermediate values directly."
        ),
    }
    (out_dir / "cube_report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"AMD cube native recovery: {counts}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
