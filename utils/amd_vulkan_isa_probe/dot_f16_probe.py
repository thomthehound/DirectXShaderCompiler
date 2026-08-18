#!/usr/bin/env python3
"""Report native Radeon FP16 dot recovery from exact half-input candidates."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path


EXPECTED = {
    "dot2_f32_f16": re.compile(r"\bv_dot2(?:acc|c)?_f32_f16(?:_e32)?\b", re.I),
    "dot2_f16_f16": re.compile(r"\bv_dot2_f16_f16\b", re.I),
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
    parser.add_argument("--out-dir", default="out/amd-dot-f16-native-probe")
    parser.add_argument("--target-env", default="vulkan1.2")
    args = parser.parse_args()

    dxc = str(Path(args.dxc).resolve())
    driver_probe = str(Path(args.driver_probe).resolve())
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    shader = Path(__file__).resolve().parent / "shaders" / "dot_f16_native.hlsl"
    spv = out_dir / "dot_f16_native.spv"
    isa = out_dir / "dot_f16_native.driver.isa"

    compiled = run([
        dxc,
        "-spirv", "-T", "cs_6_2", "-E", "main",
        "-enable-16bit-types",
        f"-fspv-target-env={args.target_env}",
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
    text = isa.read_text(encoding="utf-8", errors="replace")
    results = {name: bool(pattern.search(text)) for name, pattern in EXPECTED.items()}
    report = {
        "native_recovery": results,
        "isa": str(isa),
        "note": "FP16 dot availability/encoding is generation dependent; misses do not fail qualification.",
    }
    (out_dir / "dot_f16_report.json").write_text(json.dumps(report, indent=2) + "\n")
    for name, found in results.items():
        print(f"{name:<18}: {'NATIVE' if found else 'not found'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
