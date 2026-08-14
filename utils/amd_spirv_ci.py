#!/usr/bin/env python3
"""Run the AMD SPIR-V contract regressions without LLVM test utilities."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Result:
    argv: list[str]
    returncode: int
    stdout: str
    stderr: str


def run(argv: list[str]) -> Result:
    proc = subprocess.run(
        argv,
        text=True,
        encoding="utf-8",
        errors="replace",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    return Result(argv, proc.returncode, proc.stdout, proc.stderr)


def fail(name: str, result: Result, reason: str) -> None:
    command = " ".join(result.argv)
    output = (result.stdout + "\n" + result.stderr).strip()
    if len(output) > 6000:
        output = output[-6000:]
    raise RuntimeError(
        f"{name}: {reason}\ncommand: {command}\nreturn code: "
        f"{result.returncode}\n{output}"
    )


def require_success(
    name: str,
    result: Result,
    *,
    required: tuple[str, ...] = (),
    forbidden: tuple[str, ...] = (),
    counts: tuple[tuple[str, int], ...] = (),
) -> None:
    if result.returncode != 0:
        fail(name, result, "compilation failed")
    for pattern in required:
        if re.search(pattern, result.stdout, re.MULTILINE) is None:
            fail(name, result, f"missing pattern: {pattern}")
    for pattern in forbidden:
        if re.search(pattern, result.stdout, re.MULTILINE) is not None:
            fail(name, result, f"forbidden pattern found: {pattern}")
    for pattern, expected in counts:
        actual = len(re.findall(pattern, result.stdout, re.MULTILINE))
        if actual != expected:
            fail(name, result, f"pattern {pattern} occurred {actual}, expected {expected}")
    print(f"PASS {name}")


def require_failure(name: str, result: Result, required: str) -> None:
    if result.returncode == 0:
        fail(name, result, "compilation unexpectedly succeeded")
    output = result.stdout + "\n" + result.stderr
    if required not in output:
        fail(name, result, f"missing diagnostic: {required}")
    print(f"PASS {name}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxc", required=True)
    parser.add_argument("--tests-root")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[1]
    tests = Path(args.tests_root) if args.tests_root else (
        repo_root / "tools/clang/test/CodeGenSPIRV"
    )
    dxc = str(Path(args.dxc).resolve())

    def compile_shader(shader: str, *flags: str) -> Result:
        return run([dxc, *flags, str(tests / shader)])

    extinst_ops = (
        "FMin3AMD", "UMin3AMD", "SMin3AMD",
        "FMax3AMD", "UMax3AMD", "SMax3AMD",
        "FMid3AMD", "UMid3AMD", "SMid3AMD",
        "CubeFaceIndexAMD", "CubeFaceCoordAMD", "TimeAMD",
        "SwizzleInvocationsAMD", "SwizzleInvocationsMaskedAMD",
        "WriteInvocationAMD", "MbcntAMD",
    )
    require_success(
        "AMD extinst header",
        compile_shader(
            "amd.intrinsics.header.hlsl",
            "-T", "cs_6_2", "-E", "main", "-fcgl", "-spirv",
            "-fspv-extension=AMD",
        ),
        required=tuple(rf"\bOpExtInst\b.*\b{op}\b" for op in extinst_ops),
    )

    require_success(
        "AMD explicit vertex interpolation",
        compile_shader(
            "amd.intrinsics.explicit-vertex.hlsl",
            "-T", "ps_6_0", "-E", "main", "-fcgl", "-spirv",
            "-fspv-extension=AMD",
        ),
        required=(
            r'OpExtension "SPV_AMD_shader_explicit_vertex_parameter"',
            r"\bOpExtInst\b.*\bInterpolateAtVertexAMD\b",
        ),
    )

    require_success(
        "AMD extension family alias",
        compile_shader(
            "extensions.amd-family.hlsl",
            "-T", "vs_6_0", "-E", "main", "-fcgl", "-spirv",
            "-fspv-extension=AMD",
        ),
        required=(r'OpExtension "SPV_AMD_shader_trinary_minmax"',),
    )

    require_success(
        "AMD msad4 candidate",
        compile_shader(
            "intrinsics.msad4.amd.hlsl",
            "-T", "vs_6_0", "-E", "main", "-fcgl", "-spirv",
            "-fspv-enable-amd-intrinsics",
        ),
        required=(
            r"OpCapability DotProduct",
            r"OpCapability DotProductInput4x8BitPacked",
            r'OpExtension "SPV_KHR_integer_dot_product"',
        ),
        forbidden=(r"\bSAbs\b",),
        counts=((r"\bOpUDot\b", 4),),
    )

    require_failure(
        "strict-native msad4 contract",
        compile_shader(
            "intrinsics.msad4.amd.hlsl",
            "-T", "vs_6_0", "-E", "main", "-fcgl", "-spirv",
            "-fspv-require-native-intrinsics",
        ),
        "msad4 has a native AMD mapping to v_mqsad_u32_u8",
    )

    require_success(
        "KHR cooperative matrix",
        compile_shader(
            "linalg.cooperative-matrix.amd.hlsl",
            "-T", "cs_6_10", "-E", "main", "-fcgl", "-spirv",
            "-fspv-target-env=vulkan1.3",
        ),
        required=(
            r'OpExtension "SPV_KHR_cooperative_matrix"',
            r"\bOpCooperativeMatrixMulAddKHR\b",
        ),
    )

    require_failure(
        "Thread-scope no-fake-AMD contract",
        compile_shader(
            "linalg.thread-scope-no-fake-amd.hlsl",
            "-T", "cs_6_10", "-E", "main", "-fcgl", "-spirv",
            "-fspv-target-env=vulkan1.3",
        ),
        "thread-scope matrices require cooperative-vector semantics",
    )

    print("AMD SPIR-V backend contracts: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"FAIL {error}", file=sys.stderr)
        raise SystemExit(1)
