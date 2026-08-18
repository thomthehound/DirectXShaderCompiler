#!/usr/bin/env python3
"""Qualify exact FP16/BF16 mixed-dot SPIR-V contracts on Radeon."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path


CASES = (
    {"mode": 0, "name": "f16-acc-f32", "kind": "fp16", "gold": r"\bv_dot2_f32_f16\b"},
    {"mode": 1, "name": "f16-acc-f16", "kind": "fp16", "gold": r"\bv_dot2_f16_f16\b"},
    {"mode": 2, "name": "bf16-acc-f32", "kind": "bf16", "gold": r"\bv_dot2_f32_bf16\b"},
    {"mode": 3, "name": "bf16-acc-bf16", "kind": "bf16", "gold": r"\bv_dot2_bf16_bf16\b"},
    {"mode": 4, "name": "bf16-dot-bf16", "kind": "bf16", "gold": r"\bv_dot2_bf16_bf16\b"},
)

FAMILIES = {
    "dot2": re.compile(r"\bv_dot2[A-Za-z0-9_]*\b", re.I),
    "dot4": re.compile(r"\bv_dot4[A-Za-z0-9_]*\b", re.I),
    "fma_mix": re.compile(r"\bv_fma_mix[A-Za-z0-9_]*\b", re.I),
    "fma": re.compile(r"\bv_fma[A-Za-z0-9_]*\b", re.I),
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


def classify(proc: subprocess.CompletedProcess[str]) -> str:
    text = proc.stdout + "\n" + proc.stderr
    for marker, label in (
        ("UNSUPPORTED_FEATURE:", "feature-unsupported"),
        ("UNSUPPORTED:", "extension-or-sdk-unsupported"),
        ("PIPELINE_REJECTED:", "pipeline-rejected"),
        ("NO_DISASSEMBLY:", "no-disassembly"),
    ):
        if marker in text:
            return label
    if proc.returncode == 0:
        return "isa-produced"
    return f"probe-error:{proc.returncode}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxc", required=True)
    parser.add_argument("--mixed-dot-probe", required=True)
    parser.add_argument("--out-dir", default="out/amd-mixed-dot-probe")
    parser.add_argument("--target-env", default="vulkan1.3")
    args = parser.parse_args()

    dxc = str(Path(args.dxc).resolve())
    probe = str(Path(args.mixed_dot_probe).resolve())
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    shader = Path(__file__).resolve().parent / "shaders/mixed_dot_native.hlsl"

    results: list[dict[str, object]] = []
    hard_failure = False
    for case in CASES:
        spv = out_dir / f"{case['name']}.spv"
        isa = out_dir / f"{case['name']}.driver.isa"
        compiled = run([
            dxc, "-spirv", "-T", "cs_6_2", "-E", "main",
            "-enable-16bit-types", f"-fspv-target-env={args.target_env}",
            f"-DMODE={case['mode']}", str(shader), "-Fo", str(spv),
        ])
        if compiled.returncode != 0:
            print(f"[{case['name']}] DXC failed: {compiled.returncode}")
            print(compiled.stdout, end="")
            print(compiled.stderr, end="")
            results.append({
                "case": case["name"],
                "compile_returncode": compiled.returncode,
                "qualification": "compile-failed",
            })
            hard_failure = True
            continue

        probed = run([
            probe, str(spv), str(isa), "--kind", str(case["kind"]),
        ])
        qualification = classify(probed)
        text = isa.read_text(encoding="utf-8", errors="replace") if isa.exists() else ""
        gold = bool(re.search(str(case["gold"]), text, re.I))
        families = {
            name: sorted(set(pattern.findall(text)))
            for name, pattern in FAMILIES.items()
        }
        print(
            f"[{case['name']}] {qualification}; gold="
            f"{'FOUND' if gold else 'not found'}"
        )
        if probed.stdout:
            print(probed.stdout, end="")
        if probed.stderr:
            print(probed.stderr, end="")

        results.append({
            "case": case["name"],
            "mode": case["mode"],
            "kind": case["kind"],
            "compile_returncode": compiled.returncode,
            "probe_returncode": probed.returncode,
            "qualification": qualification,
            "gold_pattern": case["gold"],
            "gold_recovered": gold,
            "native_families": families,
            "probe_stdout": probed.stdout,
            "probe_stderr": probed.stderr,
            "spv": str(spv),
            "isa": str(isa) if isa.exists() else None,
        })
        if qualification.startswith("probe-error:"):
            hard_failure = True

    report = {
        "schema": 1,
        "shader": str(shader),
        "cases": results,
        "note": (
            "Extension/feature unsupported, pipeline rejected, and no-disassembly "
            "are qualification outcomes rather than harness failures. Gold Radeon "
            "results are v_dot2_f32_f16, v_dot2_f16_f16, v_dot2_f32_bf16 and "
            "v_dot2_bf16_bf16 where the target generation exposes them; other "
            "specialized dot/FMA-mix lowering is retained as alternative evidence."
        ),
    }
    (out_dir / "mixed_dot_report.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    return 5 if hard_failure else 0


if __name__ == "__main__":
    raise SystemExit(main())
