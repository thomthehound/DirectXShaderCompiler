#!/usr/bin/env python3
"""Run AMD/KHR subgroup contract regressions with only a built dxc executable."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxc", required=True)
    parser.add_argument("--tests-root")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    tests = Path(args.tests_root) if args.tests_root else (
        root / "tools/clang/test/CodeGenSPIRV"
    )
    dxc = str(Path(args.dxc).resolve())
    shader = tests / "amd.subgroup.rotate.hlsl"

    proc = subprocess.run(
        [
            dxc,
            "-T", "cs_6_0",
            "-E", "main",
            "-fcgl",
            "-spirv",
            "-fspv-target-env=vulkan1.1",
            str(shader),
        ],
        text=True,
        encoding="utf-8",
        errors="replace",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    output = proc.stdout + "\n" + proc.stderr
    if proc.returncode != 0:
        raise RuntimeError(f"subgroup rotate compilation failed\n{output[-8000:]}")

    required = (
        r"OpCapability GroupNonUniformRotateKHR",
        r'OpExtension "SPV_KHR_subgroup_rotate"',
    )
    for pattern in required:
        if re.search(pattern, proc.stdout, re.MULTILINE) is None:
            raise RuntimeError(f"subgroup rotate missing pattern: {pattern}")
    count = len(re.findall(r"\bOpGroupNonUniformRotateKHR\b", proc.stdout))
    if count != 4:
        raise RuntimeError(f"subgroup rotate op count {count}, expected 4")

    print("PASS AMD subgroup: exact KHR rotate + clustered rotate")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"FAIL {error}", file=sys.stderr)
        raise SystemExit(1)
