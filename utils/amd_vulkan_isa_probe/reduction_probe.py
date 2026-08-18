#!/usr/bin/env python3
"""Compare direct subgroup reductions with APUSR-style butterfly reductions."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path


SCENARIOS = (
    ("direct", "reduction_direct.hlsl"),
    ("butterfly", "reduction_butterfly.hlsl"),
)

ROUTING = {
    "dpp": re.compile(r"\b(?:dpp|quad_perm|row_sh[lr]|row_ror|wave_sh[lr]|row_bcast)\b", re.I),
    "permlane": re.compile(r"\bv_permlane[A-Za-z0-9_]*\b", re.I),
    "ds_route": re.compile(r"\bds_(?:bpermute|permute|swizzle)_b32\b", re.I),
}
VALU = re.compile(r"\bv_[A-Za-z0-9_]+\b", re.I)
SALU = re.compile(r"\bs_[A-Za-z0-9_]+\b", re.I)
DS = re.compile(r"\bds_[A-Za-z0-9_]+\b", re.I)


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


def summarize(text: str) -> dict[str, object]:
    return {
        "routing_counts": {name: len(pattern.findall(text)) for name, pattern in ROUTING.items()},
        "mnemonic_occurrences": {
            "valu": len(VALU.findall(text)),
            "salu": len(SALU.findall(text)),
            "ds": len(DS.findall(text)),
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxc", required=True)
    parser.add_argument("--driver-probe", required=True)
    parser.add_argument("--out-dir", default="out/amd-reduction-native-probe")
    parser.add_argument("--target-env", default="vulkan1.2")
    args = parser.parse_args()

    dxc = str(Path(args.dxc).resolve())
    driver_probe = str(Path(args.driver_probe).resolve())
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    shader_root = Path(__file__).resolve().parent / "shaders"

    results: dict[str, object] = {}
    for name, shader_name in SCENARIOS:
        shader = shader_root / shader_name
        spv = out_dir / f"reduction_{name}.spv"
        isa = out_dir / f"reduction_{name}.driver.isa"
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

        summary = summarize(isa.read_text(encoding="utf-8", errors="replace"))
        summary.update({"shader": str(shader), "spv": str(spv), "isa": str(isa)})
        results[name] = summary
        print(f"[{name}] routing={summary['routing_counts']} mnemonics={summary['mnemonic_occurrences']}")

    report = {
        "scenarios": results,
        "interpretation": (
            "This is an A/B codegen report, not a pass/fail optimization test. Both shaders "
            "compute unsigned sum/min/max across a fully active wave. Direct uses subgroup "
            "reduction contracts; butterfly mirrors APUSR's explicit XOR tree. Mnemonic counts "
            "are a coarse comparison only and must be paired with correctness/performance data "
            "before replacing a consumer implementation."
        ),
    }
    (out_dir / "reduction_report.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
