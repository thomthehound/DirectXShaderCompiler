#!/usr/bin/env python3
"""Spray semantically-defined HLSL patterns at the Radeon Vulkan compiler.

This is intentionally empirical. A candidate is interesting when the installed
Radeon compiler selects a specialized AMD instruction for it. We do not infer
undocumented instruction semantics from names, and a miss is not a failure.
"""

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
    family: str


CANDIDATES = (
    Candidate(0, "concat64_a_b_byte_window", "alignbyte"),
    Candidate(1, "concat64_b_a_byte_window", "alignbyte"),
    Candidate(2, "shift_or_a_b_byte_window", "alignbyte"),
    Candidate(3, "shift_or_b_a_byte_window", "alignbyte"),
    Candidate(4, "concat64_a_b_bit_window", "alignbit"),
    Candidate(5, "concat64_b_a_bit_window", "alignbit"),
    Candidate(6, "shift_or_a_b_bit_window", "alignbit"),
    Candidate(7, "shift_or_b_a_bit_window", "alignbit"),
    Candidate(8, "fixed_roll_1_byte", "alignbyte-perm"),
    Candidate(9, "fixed_roll_2_byte", "alignbyte-perm"),
    Candidate(10, "fixed_roll_3_byte", "alignbyte-perm"),
    Candidate(11, "reverse_bytes", "perm"),
    Candidate(12, "interleave_low_bytes", "perm"),
    Candidate(13, "interleave_high_bytes", "perm"),
    Candidate(14, "runtime_two_source_byte_select", "perm"),
    Candidate(15, "fixed_cross_source_perm_a", "perm"),
    Candidate(16, "fixed_cross_source_perm_b", "perm"),
    Candidate(17, "masked_two_source_select", "boolean"),
    Candidate(18, "three_input_select", "boolean"),
    Candidate(19, "packed_byte_average", "packed-byte"),
)

SPECIALIZED = {
    "alignbyte": re.compile(r"\bv_alignbyte_b32\b", re.I),
    "alignbit": re.compile(r"\bv_alignbit_b32\b", re.I),
    "perm": re.compile(r"\bv_perm_b32\b", re.I),
    "bfi": re.compile(r"\bv_bfi_b32\b", re.I),
    "lerp_u8": re.compile(r"\bv_lerp_u8\b", re.I),
    "and_or": re.compile(r"\bv_and_or_b32\b", re.I),
    "or3": re.compile(r"\bv_or3_b32\b", re.I),
    "bitop3": re.compile(r"\bv_bitop3_b32\b", re.I),
    "lshl_or": re.compile(r"\bv_lshl_or_b32\b", re.I),
    "bfe_u32": re.compile(r"\bv_bfe_u32\b", re.I),
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
    parser.add_argument("--runtime-verifier")
    parser.add_argument("--out-dir", default="out/amd-candidate-zoo")
    parser.add_argument("--target-env", default="vulkan1.2")
    parser.add_argument(
        "--candidate",
        type=int,
        action="append",
        help="Run only this APUSR_CANDIDATE index; repeat to select multiple.",
    )
    args = parser.parse_args()

    dxc = str(Path(args.dxc).resolve())
    driver_probe = str(Path(args.driver_probe).resolve())
    runtime_verifier = (
        str(Path(args.runtime_verifier).resolve()) if args.runtime_verifier else None
    )
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    shader = Path(__file__).resolve().parent / "shaders" / "candidate_zoo.hlsl"

    selected = set(args.candidate or [])
    candidates = [c for c in CANDIDATES if not selected or c.index in selected]
    unknown = selected - {c.index for c in CANDIDATES}
    if unknown:
        raise SystemExit(f"unknown candidate index(es): {sorted(unknown)}")

    records: list[dict] = []
    hits: list[dict] = []
    runtime_failures = 0

    for candidate in candidates:
        stem = f"{candidate.index:02d}_{candidate.name}"
        spv = out_dir / f"{stem}.spv"
        isa = out_dir / f"{stem}.isa"
        compiled = run([
            dxc,
            "-spirv", "-T", "cs_6_2", "-E", "main",
            f"-fspv-target-env={args.target_env}",
            f"-DAPUSR_CANDIDATE={candidate.index}",
            str(shader), "-Fo", str(spv),
        ])
        if compiled.returncode != 0:
            records.append({
                "index": candidate.index,
                "name": candidate.name,
                "family": candidate.family,
                "compile": "failed",
                "stderr": compiled.stderr[-4000:],
            })
            print(f"{candidate.index:02d} {candidate.name:<34} COMPILE FAIL")
            continue

        runtime_status = "not-run"
        runtime_output = ""
        if runtime_verifier is not None:
            verified = run([runtime_verifier, str(spv), str(candidate.index)])
            runtime_output = (verified.stdout + "\n" + verified.stderr).strip()
            if verified.returncode == 0:
                runtime_status = "pass"
            else:
                runtime_status = "failed"
                runtime_failures += 1

        probed = run([driver_probe, str(spv), str(isa)])
        if probed.returncode != 0 or not isa.exists():
            records.append({
                "index": candidate.index,
                "name": candidate.name,
                "family": candidate.family,
                "compile": "pass",
                "runtime": runtime_status,
                "runtime_output": runtime_output[-4000:],
                "probe": "failed",
                "stdout": probed.stdout[-4000:],
                "stderr": probed.stderr[-4000:],
            })
            print(
                f"{candidate.index:02d} {candidate.name:<34} "
                f"runtime={runtime_status} PROBE FAIL"
            )
            continue

        text = isa.read_text(encoding="utf-8", errors="replace")
        found = [name for name, pattern in SPECIALIZED.items() if pattern.search(text)]
        record = {
            "index": candidate.index,
            "name": candidate.name,
            "family": candidate.family,
            "compile": "pass",
            "runtime": runtime_status,
            "runtime_output": runtime_output[-4000:],
            "probe": "pass",
            "specialized": found,
            "spv": str(spv),
            "isa": str(isa),
        }
        records.append(record)
        if found and runtime_status != "failed":
            hits.append(record)
        label = ", ".join(found) if found else "no targeted mnemonic"
        print(
            f"{candidate.index:02d} {candidate.name:<34} "
            f"runtime={runtime_status:<7} {label}"
        )

    report = {
        "shader": str(shader),
        "candidate_count": len(candidates),
        "hit_count": len(hits),
        "runtime_failures": runtime_failures,
        "hits": hits,
        "records": records,
        "policy": (
            "A promoted hit requires a specialized Radeon ISA mnemonic and no "
            "runtime semantic failure. It still does not license substituting the "
            "expression into APUSR until the relevant APUSR consumer passes a "
            "differential/numerical and performance A/B test."
        ),
    }
    (out_dir / "candidate_zoo_report.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    (out_dir / "candidate_zoo_hits.json").write_text(
        json.dumps(hits, indent=2) + "\n", encoding="utf-8"
    )

    print(
        f"candidate zoo: {len(hits)} promoted hit(s) / {len(candidates)} candidate(s); "
        f"runtime failures={runtime_failures}"
    )
    # Empirical misses are useful data. A runtime semantic mismatch is not.
    return 1 if runtime_failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
