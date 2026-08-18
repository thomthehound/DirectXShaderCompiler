#!/usr/bin/env python3
"""Compile APUSR cross-lane candidates and classify Radeon native routing."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path


FAMILIES = {
    "dpp": re.compile(r"\b(?:dpp|quad_perm|row_sh[lr]|row_ror|wave_sh[lr]|row_bcast)\b", re.I),
    "permlane": re.compile(r"\bv_permlane(?:16|x16|64|_bcast|_xor|_up|_down)[A-Za-z0-9_]*\b", re.I),
    "ds_bpermute": re.compile(r"\bds_bpermute_b32\b", re.I),
    "ds_permute": re.compile(r"\bds_permute_b32\b", re.I),
    "ds_swizzle": re.compile(r"\bds_swizzle_b32\b", re.I),
}

SCENARIOS = (
    ("xor_butterfly", "crosslane_native.hlsl"),
    ("neighbor_row", "crosslane_neighbor.hlsl"),
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


def classify(text: str) -> dict[str, object]:
    counts = {name: len(pattern.findall(text)) for name, pattern in FAMILIES.items()}
    return {
        "specialized_routing_counts": counts,
        "any_specialized_crosslane": any(counts.values()),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxc", required=True)
    parser.add_argument("--driver-probe", required=True)
    parser.add_argument("--out-dir", default="out/amd-crosslane-native-probe")
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
        spv = out_dir / f"{name}.spv"
        isa = out_dir / f"{name}.driver.isa"

        compiled = run([
            dxc, "-spirv", "-T", "cs_6_0", "-E", "main",
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
        result = classify(text)
        result.update({"shader": str(shader), "spv": str(spv), "isa": str(isa)})
        scenarios[name] = result

        counts = result["specialized_routing_counts"]
        print(f"[{name}]")
        for family, count in counts.items():
            print(f"  {family:<12} : {count}")

    report = {
        "scenarios": scenarios,
        "note": (
            "The exact routing mnemonic is generation/driver dependent. DPP, permlane, "
            "or DS routing are all specialized evidence. Each scenario is reported "
            "separately so a butterfly hit cannot hide a failed neighbour/row combine."
        ),
    }
    (out_dir / "crosslane_report.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
