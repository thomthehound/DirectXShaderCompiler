#!/usr/bin/env python3
"""Qualify APUSR-relevant cooperative-matrix properties and Radeon ISA."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path


CASES = (
    {"mode": 0, "name": "f16xf16-f32", "a": "f16", "b": "f16", "c": "f32", "r": "f32",
     "expected": r"\bv_wmma_f32_16x16x16_f16\b", "extra": []},
    {"mode": 1, "name": "f16xf16-f16", "a": "f16", "b": "f16", "c": "f16", "r": "f16",
     "expected": r"\bv_wmma_f16_16x16x16_f16\b", "extra": []},
    {"mode": 2, "name": "bf16xbf16-f32", "a": "bf16", "b": "bf16", "c": "f32", "r": "f32",
     "expected": r"\bv_wmma_f32_16x16x16_bf16\b", "extra": ["-fspv-extension=SPV_KHR_bfloat16"]},
    {"mode": 3, "name": "bf16xbf16-bf16", "a": "bf16", "b": "bf16", "c": "bf16", "r": "bf16",
     "expected": r"\bv_wmma_bf16_16x16x16_bf16\b", "extra": ["-fspv-extension=SPV_KHR_bfloat16"]},
    {"mode": 4, "name": "i8xi8-i32", "a": "i8", "b": "i8", "c": "i32", "r": "i32",
     "expected": r"\bv_wmma_i32_16x16x16_iu8\b", "extra": []},
    {"mode": 5, "name": "u8xu8-u32", "a": "u8", "b": "u8", "c": "u32", "r": "u32",
     "expected": r"\bv_wmma_i32_16x16x16_iu8\b", "extra": []},
    {"mode": 6, "name": "i8xu8-i32", "a": "i8", "b": "u8", "c": "i32", "r": "i32",
     "expected": r"\bv_wmma_i32_16x16x16_iu8\b", "extra": []},
    {"mode": 7, "name": "u8xi8-i32", "a": "u8", "b": "i8", "c": "i32", "r": "i32",
     "expected": r"\bv_wmma_i32_16x16x16_iu8\b", "extra": []},
)

FAMILY_PATTERNS = {
    "wmma": re.compile(r"\bv_wmma_[A-Za-z0-9_]+\b", re.I),
    "swmmac": re.compile(r"\bv_swmmac_[A-Za-z0-9_]+\b", re.I),
    "mfma": re.compile(r"\bv_mfma_[A-Za-z0-9_]+\b", re.I),
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


def classify_probe(proc: subprocess.CompletedProcess[str]) -> str:
    text = proc.stdout + "\n" + proc.stderr
    for marker, label in (
        ("UNSUPPORTED_PROPERTY:", "property-unsupported"),
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
    parser.add_argument("--matrix-probe", required=True)
    parser.add_argument("--out-dir", default="out/amd-matrix-probe")
    parser.add_argument("--target-env", default="vulkan1.3")
    args = parser.parse_args()

    dxc = str(Path(args.dxc).resolve())
    matrix_probe = str(Path(args.matrix_probe).resolve())
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    shader = Path(__file__).resolve().parents[2] / "tools/clang/test/CodeGenSPIRV/linalg.cooperative-matrix-shapes.amd.hlsl"

    results: list[dict[str, object]] = []
    hard_failure = False
    for case in CASES:
        spv = out_dir / f"{case['name']}.spv"
        isa = out_dir / f"{case['name']}.driver.isa"
        compile_proc = run([
            dxc, "-spirv", "-T", "cs_6_10", "-E", "main",
            f"-fspv-target-env={args.target_env}", *case["extra"],
            f"-DMODE={case['mode']}", str(shader), "-Fo", str(spv),
        ])
        if compile_proc.returncode != 0:
            print(f"[{case['name']}] DXC failed: {compile_proc.returncode}")
            print(compile_proc.stdout, end="")
            print(compile_proc.stderr, end="")
            results.append({
                "case": case["name"], "compile_returncode": compile_proc.returncode,
                "qualification": "compile-failed",
            })
            hard_failure = True
            continue

        probe_proc = run([
            matrix_probe, str(spv), str(isa),
            "--a-type", str(case["a"]), "--b-type", str(case["b"]),
            "--c-type", str(case["c"]), "--result-type", str(case["r"]),
            "--m", "16", "--n", "16", "--k", "16",
        ])
        qualification = classify_probe(probe_proc)
        text = isa.read_text(encoding="utf-8", errors="replace") if isa.exists() else ""
        families = {
            name: sorted(set(pattern.findall(text)))
            for name, pattern in FAMILY_PATTERNS.items()
        }
        expected_native = bool(re.search(str(case["expected"]), text, re.I))
        print(
            f"[{case['name']}] {qualification}; expected-wmma="
            f"{'FOUND' if expected_native else 'not found'}"
        )
        if probe_proc.stdout:
            print(probe_proc.stdout, end="")
        if probe_proc.stderr:
            print(probe_proc.stderr, end="")

        results.append({
            "case": case["name"],
            "mode": case["mode"],
            "types": {"a": case["a"], "b": case["b"], "c": case["c"], "result": case["r"]},
            "shape": {"m": 16, "n": 16, "k": 16},
            "compile_returncode": compile_proc.returncode,
            "probe_returncode": probe_proc.returncode,
            "qualification": qualification,
            "expected_wmma_recovered": expected_native,
            "native_families": families,
            "probe_stdout": probe_proc.stdout,
            "probe_stderr": probe_proc.stderr,
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
            "Unsupported extension/feature/property and pipeline-rejected are qualification "
            "results, not harness failures. A successful property match plus expected WMMA "
            "mnemonic is the strongest installed-driver result. MFMA/SWMMAC are retained as "
            "specialized alternative evidence rather than rejected."
        ),
    }
    (out_dir / "matrix_report.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    return 5 if hard_failure else 0


if __name__ == "__main__":
    raise SystemExit(main())
