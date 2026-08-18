#!/usr/bin/env python3
"""Compare scalar and vector groupshared access spellings in Radeon ISA."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path

SCENARIOS = (
    ("scalar", "lds_scalar.hlsl"),
    ("vector", "lds_vector.hlsl"),
)
DS_RE = re.compile(r"\bds_[A-Za-z0-9_]+\b", re.I)
BARRIER_RE = re.compile(r"\bs_barrier\b", re.I)
WAIT_RE = re.compile(r"\bs_waitcnt(?:_depctr)?\b", re.I)


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
    ds: dict[str, int] = {}
    for name in DS_RE.findall(text):
        key = name.lower()
        ds[key] = ds.get(key, 0) + 1
    return {
        "ds_instruction_counts": dict(sorted(ds.items())),
        "ds_total": sum(ds.values()),
        "barrier_count": len(BARRIER_RE.findall(text)),
        "wait_count": len(WAIT_RE.findall(text)),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxc", required=True)
    parser.add_argument("--driver-probe", required=True)
    parser.add_argument("--out-dir", default="out/amd-lds-probe")
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
        spv = out_dir / f"lds_{name}.spv"
        isa = out_dir / f"lds_{name}.driver.isa"
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
        scenarios[name] = summary
        print(
            f"[{name}] ds_total={summary['ds_total']} "
            f"barriers={summary['barrier_count']} waits={summary['wait_count']}"
        )
        for mnemonic, count in summary["ds_instruction_counts"].items():
            print(f"  {mnemonic:<24} {count}")

    report = {
        "scenarios": scenarios,
        "interpretation": (
            "The scalar and vector shaders have identical logical synchronization and "
            "per-lane data. Wider/fewer DS instructions are useful codegen evidence, not "
            "an automatic product change. Runtime data-integrity and APUSR tile comparison "
            "remain required before consumer rewrites."
        ),
    }
    (out_dir / "lds_report.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
