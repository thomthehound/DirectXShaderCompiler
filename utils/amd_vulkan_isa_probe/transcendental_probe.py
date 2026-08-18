#!/usr/bin/env python3
"""Compare direct and exp/log transcendental spellings in Radeon ISA."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path

SCENARIOS = (
    ("direct", "transcendental_direct.hlsl"),
    ("exp_log", "transcendental_exp_log.hlsl"),
)
EXPECTED = {
    "tanh": re.compile(r"\bv_tanh_[A-Za-z0-9_]+\b", re.I),
    "log": re.compile(r"\bv_log_[A-Za-z0-9_]+\b", re.I),
    "exp": re.compile(r"\bv_exp_[A-Za-z0-9_]+\b", re.I),
    "rcp": re.compile(r"\bv_rcp_[A-Za-z0-9_]+\b", re.I),
    "frexp_mant": re.compile(r"\bv_frexp_mant_[A-Za-z0-9_]+\b", re.I),
    "frexp_exp": re.compile(r"\bv_frexp_exp_[A-Za-z0-9_]+\b", re.I),
    "ldexp": re.compile(r"\bv_ldexp_[A-Za-z0-9_]+\b", re.I),
}
VALU = re.compile(r"\bv_[A-Za-z0-9_]+\b", re.I)


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
        "specialized_counts": {name: len(pattern.findall(text)) for name, pattern in EXPECTED.items()},
        "valu_mnemonic_occurrences": len(VALU.findall(text)),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxc", required=True)
    parser.add_argument("--driver-probe", required=True)
    parser.add_argument("--out-dir", default="out/amd-transcendental-probe")
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
        spv = out_dir / f"transcendental_{name}.spv"
        isa = out_dir / f"transcendental_{name}.driver.isa"
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

        summary = summarize(isa.read_text(encoding="utf-8", errors="replace"))
        summary.update({"shader": str(shader), "spv": str(spv), "isa": str(isa)})
        scenarios[name] = summary
        print(
            f"[{name}] specialized={summary['specialized_counts']} "
            f"VALU={summary['valu_mnemonic_occurrences']}"
        )

    report = {
        "scenarios": scenarios,
        "interpretation": (
            "exp_log is an empirical numerical candidate, not a semantic replacement. "
            "Its shader constrains inputs to a domain where the mathematical identities "
            "are valid, but finite-precision error still requires a dense APUSR-oriented "
            "numerical oracle before promotion. This report only compares native codegen."
        ),
    }
    (out_dir / "transcendental_report.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
