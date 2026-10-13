#!/usr/bin/env python3
"""Run a bounded, independently maintained upstream DXC LIT/FileCheck gate.

The selected upstream tests cover ordinary DXIL, command-line behavior,
KHR cooperative-matrix SPIR-V, and a target-environment diagnostic. They are
real LIT RUN lines executed by the repository's own test infrastructure, not
equivalent AMD-specific Python assertions.

Run after building dxc, dxcompiler, dxildll, FileCheck, not, and llvm-config with
LLVM_INCLUDE_TESTS=ON and CLANG_INCLUDE_TESTS=ON.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TEST_ROOT = ROOT / "tools" / "clang" / "test"
CASES = (
    "DXC/basic_smoke.test",
    "CodeGenDXIL/cbuffer.namespace.hlsl",
    "CodeGenSPIRV/coopmatrix_muladd_test.hlsl",
    "CodeGenSPIRV/coopmatrix.groupshared.hlsl",
    "CodeGenSPIRV/coopmatrix.old.spirv.hlsl",
    "CodeGenSPIRV/amd.intrinsics.vertex-parameter-index.hlsl",
    "CodeGenSPIRV/amd.intrinsics.vertex-parameter-index.invalid.hlsl",
    "CodeGenSPIRV/amd.experimental-option-scope.hlsl",
    "CodeGenSPIRV/amd.intrinsics.vertex-parameter-out-of-range.hlsl",
    "CodeGenSPIRV/amd.intrinsics.vertex-parameter-helper.hlsl",
    "CodeGenSPIRV/amd.intrinsics.vertex-parameter-shared-helper.hlsl",
    "CodeGenSPIRV/amd.intrinsics.vertex-parameter-default-argument.hlsl",
)


def check_sources() -> list[str]:
    errors = []
    for case in CASES:
        source = TEST_ROOT / case
        if not source.is_file():
            errors.append(f"Missing upstream test: {case}")
        elif not re.search(r"^\s*(//|;|#)\s*RUN:", source.read_text(
            encoding="utf-8", errors="replace"
        ), re.M):
            errors.append(f"No LIT RUN lines found in {case}")
    if len(CASES) != len(set(CASES)):
        errors.append("Duplicate upstream test selection")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--self-test", action="store_true",
                        help="Check that every selected upstream fixture exists")
    args = parser.parse_args()

    errors = check_sources()
    if errors:
        for error in errors:
            print("FAIL upstream LIT inventory: " + error, file=sys.stderr)
        return 2
    if args.self_test:
        print(f"PASS upstream LIT fixture inventory ({len(CASES)} tests)")
        return 0
    if args.build_dir is None:
        parser.error("--build-dir is required without --self-test")

    build = args.build_dir.resolve()
    site = build / "tools/clang/test/lit.site.cfg"
    if not site.is_file():
        print(f"FAIL: no configured LIT site: {site}", file=sys.stderr)
        return 2

    bin_dir = build / "bin"
    for tool in ("dxc", "FileCheck", "not", "llvm-config"):
        name = tool + (".exe" if sys.platform == "win32" else "")
        if not (bin_dir / name).is_file():
            print(f"FAIL: missing required LIT tool: {bin_dir / name}",
                  file=sys.stderr)
            return 2

    command = [
        sys.executable, str(ROOT / "utils/lit/lit.py"),
        "-v", "--param", f"clang_site_config={site}",
        *(str(TEST_ROOT / case) for case in CASES),
    ]
    env = os.environ.copy()
    library_dir = str(bin_dir if sys.platform == "win32" else build / "lib")
    key = "PATH" if sys.platform == "win32" else "LD_LIBRARY_PATH"
    env[key] = library_dir + os.pathsep + env.get(key, "")
    result = subprocess.run(
        command, cwd=ROOT, env=env, text=True, encoding="utf-8",
        errors="replace", stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        check=False,
    )
    print(result.stdout, end="" if result.stdout.endswith("\n") else "\n",
          flush=True)
    if result.returncode != 0:
        print(f"FAIL upstream LIT run (exit {result.returncode})",
              file=sys.stderr)
        return 1

    # LIT may return success with every case unsupported or skipped. This
    # gate requires each explicitly selected test to actually pass.
    passed = len(re.findall(r"(?m)^PASS:\s+Clang\s+::", result.stdout))
    if passed != len(CASES):
        print(f"FAIL upstream LIT: {passed}/{len(CASES)} tests passed; "
              "skips and unsupported tests do not qualify", file=sys.stderr)
        return 1
    print(f"PASS upstream LIT gate ({passed}/{len(CASES)} actual passes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
