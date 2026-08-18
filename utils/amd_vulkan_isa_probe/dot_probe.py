#!/usr/bin/env python3
"""Compile AMD dot contracts independently and report Radeon ISA recovery."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Candidate:
    index: int
    name: str
    expected: re.Pattern[str]


CANDIDATES = (
    Candidate(0, "dot4_i8", re.compile(r"\bv_dot4(?:c)?_i32_(?:i8|iu8)\b", re.I)),
    Candidate(1, "dot4_u8", re.compile(r"\bv_dot4(?:c)?_u32_u8\b", re.I)),
    Candidate(
        2,
        "dot4_i8_accsat",
        re.compile(r"\bv_dot4(?:c)?_i32_(?:i8|iu8)\b[^\n]*\bclamp\b", re.I),
    ),
    Candidate(
        3,
        "dot4_u8_accsat",
        re.compile(r"\bv_dot4(?:c)?_u32_u8\b[^\n]*\bclamp\b", re.I),
    ),
    Candidate(4, "dot4_i8_u8", re.compile(r"\bv_dot4_i32_iu8\b", re.I)),
    Candidate(5, "dot4_u8_i8", re.compile(r"\bv_dot4_i32_iu8\b", re.I)),
    Candidate(
        6,
        "dot4_i8_u8_accsat",
        re.compile(r"\bv_dot4_i32_iu8\b[^\n]*\bclamp\b", re.I),
    ),
    Candidate(
        7,
        "dot4_u8_i8_accsat",
        re.compile(r"\bv_dot4_i32_iu8\b[^\n]*\bclamp\b", re.I),
    ),
    Candidate(8, "dot2_i16", re.compile(r"\bv_dot2(?:c)?_i32_i16\b", re.I)),
    Candidate(9, "dot2_u16", re.compile(r"\bv_dot2(?:c)?_u32_u16\b", re.I)),
    Candidate(10, "dot8_i4", re.compile(r"\bv_dot8(?:c)?_i32_i4\b", re.I)),
    Candidate(11, "dot8_u4", re.compile(r"\bv_dot8_u32_u4\b", re.I)),
    Candidate(12, "dot8_i4_u4", re.compile(r"\bv_dot8_i32_iu4\b", re.I)),
    Candidate(13, "dot8_u4_i4", re.compile(r"\bv_dot8_i32_iu4\b", re.I)),
    Candidate(
        14,
        "dot2_f32_f16_bits",
        re.compile(r"\bv_dot2(?:acc|c)?_f32_f16(?:_e32)?\b", re.I),
    ),
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxc", required=True)
    parser.add_argument("--driver-probe", required=True)
    parser.add_argument("--out-dir", default="out/amd-dot-native-probe")
    parser.add_argument("--target-env", default="vulkan1.2")
    parser.add_argument("--candidate", type=int, action="append")
    args = parser.parse_args()

    dxc = str(Path(args.dxc).resolve())
    driver_probe = str(Path(args.driver_probe).resolve())
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    shader = Path(__file__).resolve().parent / "shaders" / "dot_native.hlsl"

    selected = set(args.candidate or [])
    candidates = [c for c in CANDIDATES if not selected or c.index in selected]
    unknown = selected - {c.index for c in CANDIDATES}
    if unknown:
        print(f"unknown dot candidate index(es): {sorted(unknown)}")
        return 2

    records: list[dict] = []
    compile_failures = 0

    for candidate in candidates:
        stem = f"{candidate.index:02d}_{candidate.name}"
        spv = out_dir / f"{stem}.spv"
        isa = out_dir / f"{stem}.driver.isa"
        compiled = run([
            dxc,
            "-spirv",
            "-T", "cs_6_4",
            "-E", "main",
            f"-fspv-target-env={args.target_env}",
            f"-DAPUSR_DOT_CANDIDATE={candidate.index}",
            str(shader),
            "-Fo", str(spv),
        ])
        if compiled.returncode != 0:
            compile_failures += 1
            records.append({
                "index": candidate.index,
                "name": candidate.name,
                "compile": "failed",
                "stderr": compiled.stderr[-5000:],
            })
            print(f"{candidate.index:02d} {candidate.name:<24} COMPILE FAIL")
            continue

        probed = run([driver_probe, str(spv), str(isa)])
        if probed.returncode != 0 or not isa.exists():
            records.append({
                "index": candidate.index,
                "name": candidate.name,
                "compile": "pass",
                "driver": "rejected-or-no-isa",
                "stdout": probed.stdout[-5000:],
                "stderr": probed.stderr[-5000:],
            })
            print(f"{candidate.index:02d} {candidate.name:<24} driver rejected/no ISA")
            continue

        text = isa.read_text(encoding="utf-8", errors="replace")
        native = bool(candidate.expected.search(text))
        records.append({
            "index": candidate.index,
            "name": candidate.name,
            "compile": "pass",
            "driver": "pass",
            "native": native,
            "spv": str(spv),
            "isa": str(isa),
        })
        print(
            f"{candidate.index:02d} {candidate.name:<24} "
            f"{'NATIVE' if native else 'not found'}"
        )

    report = {
        "shader": str(shader),
        "candidate_count": len(candidates),
        "compile_failures": compile_failures,
        "records": records,
        "policy": (
            "Each module contains exactly one dot contract, so a native mnemonic is "
            "causally attributable to that source operation. Driver rejection or a "
            "missing generation-specific mnemonic is evidence, not a harness failure; "
            "DXC compilation failure is a harness/contract failure."
        ),
    }
    (out_dir / "dot_report.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    return 1 if compile_failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
