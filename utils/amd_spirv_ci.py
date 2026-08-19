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
    if len(output) > 8000:
        output = output[-8000:]
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
        "AMD wave intrinsic Vulkan parity",
        compile_shader(
            "amd.intrinsics.wave-parity.hlsl",
            "-T", "cs_6_0", "-E", "main", "-fcgl", "-spirv",
            "-fspv-target-env=vulkan1.1",
        ),
        required=(
            r"\bOpGroupNonUniformBroadcastFirst\b",
            r"\bOpGroupNonUniformShuffle\b",
            r"\bOpGroupNonUniformBallot\b",
            r"\bOpGroupNonUniformAny\b",
            r"\bOpGroupNonUniformAll\b",
        ),
        forbidden=(r"\bSwizzleInvocationsAMD\b",),
    )

    require_success(
        "AMD wave reduce and scan parity",
        compile_shader(
            "amd.intrinsics.wave-reduce-scan.hlsl",
            "-T", "cs_6_0", "-E", "main", "-fcgl", "-spirv",
            "-fspv-target-env=vulkan1.1",
        ),
        required=(
            r"\bOpGroupNonUniformFAdd\b",
            r"\bOpGroupNonUniformIAdd\b",
            r"\bOpGroupNonUniformFMul\b",
            r"\bOpGroupNonUniformIMul\b",
            r"\bOpGroupNonUniformFMin\b",
            r"\bOpGroupNonUniformSMin\b",
            r"\bOpGroupNonUniformUMin\b",
            r"\bOpGroupNonUniformFMax\b",
            r"\bOpGroupNonUniformSMax\b",
            r"\bOpGroupNonUniformUMax\b",
            r"\bOpGroupNonUniformBitwiseAnd\b",
            r"\bOpGroupNonUniformBitwiseOr\b",
            r"\bOpGroupNonUniformBitwiseXor\b",
            r"ExclusiveScan",
        ),
    )

    require_success(
        "AMD full scalar/vector wave scan parity",
        compile_shader(
            "amd.intrinsics.wave-scan-full.hlsl",
            "-T", "cs_6_0", "-E", "main", "-fcgl", "-spirv",
            "-fspv-target-env=vulkan1.1",
        ),
        required=(
            r"\bOpGroupNonUniformFAdd\b.*\bInclusiveScan\b",
            r"\bOpGroupNonUniformIAdd\b.*\bInclusiveScan\b",
            r"\bOpGroupNonUniformFMul\b.*\bExclusiveScan\b",
            r"\bOpGroupNonUniformIMul\b.*\bExclusiveScan\b",
            r"\bOpGroupNonUniformFMin\b.*\bInclusiveScan\b",
            r"\bOpGroupNonUniformSMin\b.*\bExclusiveScan\b",
            r"\bOpGroupNonUniformUMin\b.*\bInclusiveScan\b",
            r"\bOpGroupNonUniformFMax\b.*\bExclusiveScan\b",
            r"\bOpGroupNonUniformSMax\b.*\bInclusiveScan\b",
            r"\bOpGroupNonUniformUMax\b.*\bExclusiveScan\b",
            r"\bOpGroupNonUniformBitwiseAnd\b.*\bReduce\b",
            r"\bOpGroupNonUniformBitwiseOr\b.*\bReduce\b",
            r"\bOpGroupNonUniformBitwiseXor\b.*\bReduce\b",
        ),
    )

    require_success(
        "AMD swizzle immediate parity",
        compile_shader(
            "amd.intrinsics.swizzle-parity.hlsl",
            "-T", "cs_6_0", "-E", "main", "-fcgl", "-spirv",
            "-fspv-extension=AMD",
        ),
        required=(
            r'OpExtension "SPV_AMD_shader_ballot"',
            r"\bOpExtInst\b.*\bSwizzleInvocationsMaskedAMD\b",
        ),
    )

    require_success(
        "AMD exact core math contracts",
        compile_shader(
            "amd.math.core.hlsl",
            "-T", "cs_6_2", "-E", "main", "-fcgl", "-spirv",
            "-fspv-extension=AMD",
        ),
        required=(
            r"\bOpBitFieldUExtract\b",
            r"\bOpBitFieldSExtract\b",
            r"\bOpBitReverse\b",
            r"\bOpBitCount\b",
            r"\bOpExtInst\b.*\bFMid3AMD\b",
        ),
        counts=(
            (r"\bOpBitFieldUExtract\b", 1),
            (r"\bOpBitFieldSExtract\b", 1),
            (r"\bOpBitReverse\b", 1),
            (r"\bOpBitCount\b", 1),
        ),
    )

    require_success(
        "AMD MUL24 canonical recovery structure",
        compile_shader(
            "amd.math.candidates.hlsl",
            "-T", "cs_6_2", "-E", "main", "-fcgl", "-spirv",
        ),
        required=(
            r"\bOpIMul\b",
            r"\bOpBitwiseAnd\b",
            r"\bOpShiftLeftLogical\b",
            r"\bOpShiftRightArithmetic\b",
        ),
        counts=((r"\bOpIMul\b", 2),),
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

    barycentric_modes = (
        (0, "perspective-center", r"BuiltIn BaryCoordKHR", ()),
        (1, "perspective-centroid", r"BuiltIn BaryCoordKHR", (r"OpDecorate .* Centroid",)),
        (2, "perspective-sample", r"BuiltIn BaryCoordKHR", (r"OpDecorate .* Sample",)),
        (3, "linear-center", r"BuiltIn BaryCoordNoPerspKHR", ()),
        (4, "linear-centroid", r"BuiltIn BaryCoordNoPerspKHR", (r"OpDecorate .* Centroid",)),
        (5, "linear-sample", r"BuiltIn BaryCoordNoPerspKHR", (r"OpDecorate .* Sample",)),
    )
    for mode, label, builtin, extra in barycentric_modes:
        require_success(
            f"AMD barycentric {label}",
            compile_shader(
                "amd.intrinsics.barycentric.hlsl",
                "-T", "ps_6_1", "-E", "main", "-fcgl", "-spirv",
                "-fspv-target-env=vulkan1.1",
                "-fspv-extension=SPV_KHR_fragment_shader_barycentric",
                f"-DMODE={mode}",
            ),
            required=(
                r'OpExtension "SPV_KHR_fragment_shader_barycentric"',
                builtin,
                *extra,
            ),
        )

    require_success(
        "AMD pull-model barycentric parity",
        compile_shader(
            "amd.intrinsics.pull-model-barycentric.hlsl",
            "-T", "ps_6_1", "-E", "main", "-fcgl", "-spirv",
            "-fspv-extension=AMD",
        ),
        required=(
            r'OpExtension "SPV_AMD_shader_explicit_vertex_parameter"',
            r"BuiltIn BaryCoordPullModelAMD",
        ),
    )

    require_success(
        "AMD draw parameter Vulkan parity",
        compile_shader(
            "amd.intrinsics.draw-parameters.hlsl",
            "-T", "vs_6_8", "-E", "main", "-fcgl", "-spirv",
            "-fspv-target-env=vulkan1.1",
        ),
        required=(
            r"OpCapability DrawParameters",
            r"BuiltIn BaseVertex",
            r"BuiltIn BaseInstance",
            r"BuiltIn DrawIndex",
        ),
    )

    require_success(
        "AMD shader clock parity",
        compile_shader(
            "amd.intrinsics.shader-clock.hlsl",
            "-T", "cs_6_2", "-E", "main", "-fcgl", "-spirv",
            "-fspv-target-env=vulkan1.1",
            "-fspv-extension=SPV_KHR_shader_clock",
        ),
        required=(
            r"OpCapability ShaderClockKHR",
            r'OpExtension "SPV_KHR_shader_clock"',
            r"\bOpReadClockKHR\b",
        ),
    )

    atomic_required = (
        r"OpCapability Int64",
        r"OpCapability Int64Atomics",
        r"\bOpAtomicIAdd\b",
        r"\bOpAtomicAnd\b",
        r"\bOpAtomicOr\b",
        r"\bOpAtomicXor\b",
        r"\bOpAtomicUMin\b",
        r"\bOpAtomicUMax\b",
        r"\bOpAtomicExchange\b",
        r"\bOpAtomicCompareExchange\b",
    )
    require_success(
        "AMD 64-bit buffer atomic parity",
        compile_shader(
            "amd.intrinsics.atomic-u64.hlsl",
            "-T", "cs_6_6", "-E", "main", "-fcgl", "-spirv",
            "-fspv-target-env=vulkan1.2",
        ),
        required=atomic_required + (r"\bOpAtomicSMin\b", r"\bOpAtomicSMax\b"),
    )
    require_success(
        "AMD 64-bit image atomic parity",
        compile_shader(
            "amd.intrinsics.atomic-u64-image.hlsl",
            "-T", "cs_6_6", "-E", "main", "-fcgl", "-spirv",
            "-fspv-target-env=vulkan1.2",
            "-fspv-extension=SPV_EXT_shader_image_int64",
        ),
        required=atomic_required + (
            r'OpExtension "SPV_EXT_shader_image_int64"',
            r"OpCapability Int64ImageEXT",
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

    packed_dot_required = (
        r"OpCapability DotProduct",
        r"OpCapability DotProductInput4x8BitPacked",
        r'OpExtension "SPV_KHR_integer_dot_product"',
    )
    packed_dot_counts = ((r"\bOpSDot\b", 1), (r"\bOpUDot\b", 1))
    require_success(
        "packed signed/unsigned dot contracts",
        compile_shader(
            "intrinsics.dot4add.packed.amd.hlsl",
            "-T", "vs_6_4", "-E", "main", "-fcgl", "-spirv",
        ),
        required=packed_dot_required,
        forbidden=(r"\bOpIMul\b",),
        counts=packed_dot_counts,
    )
    require_success(
        "strict-native packed dot contracts",
        compile_shader(
            "intrinsics.dot4add.packed.amd.hlsl",
            "-T", "vs_6_4", "-E", "main", "-fcgl", "-spirv",
            "-fspv-require-native-intrinsics",
        ),
        required=packed_dot_required,
        forbidden=(r"\bOpIMul\b",),
        counts=packed_dot_counts,
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
