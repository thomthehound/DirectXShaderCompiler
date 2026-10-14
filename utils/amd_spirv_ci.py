#!/usr/bin/env python3
"""Run the AMD SPIR-V contract regressions without LLVM test utilities."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

from amd_spirv_interface import check_pixel_parameter_interface


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

    require_success(
        "AMD header registration only",
        compile_shader(
            "amd.intrinsics.header-registration.hlsl",
            "-T", "cs_6_2", "-E", "main", "-spirv",
            "-fspv-extension=AMD",
        ),
    )

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
        "AMD raw vertex-parameter index parity",
        compile_shader(
            "amd.intrinsics.vertex-parameter-index.hlsl",
            "-T", "ps_6_0", "-E", "main", "-fcgl", "-spirv",
            "-fspv-extension=AMD",
        ),
        required=(
            r'OpExtension "SPV_AMD_shader_explicit_vertex_parameter"',
            r"\bInterpolateAtVertexAMD\b",
            r"Location 7\b",
            r"Location 9\b",
        ),
        counts=((r"\bInterpolateAtVertexAMD\b", 4),),
    )

    require_success(
        "AMD raw vertex-parameter helper with one entry",
        compile_shader(
            "amd.intrinsics.vertex-parameter-helper.hlsl",
            "-T", "ps_6_0", "-E", "main", "-fcgl", "-spirv",
            "-fspv-extension=AMD",
        ),
        required=(r'OpExtension "SPV_AMD_shader_explicit_vertex_parameter"',),
        counts=((r"\bInterpolateAtVertexAMD\b", 4),),
    )
    unique_multi_entry = compile_shader(
        "amd.intrinsics.vertex-parameter-shared-helper.hlsl",
        "-T", "lib_6_6", "-E", "main", "-fcgl", "-spirv",
        "-fspv-extension=AMD", "-DUNIQUE_OWNER",
    )
    require_success(
        "AMD raw vertex-parameter local helper in multi-entry library",
        unique_multi_entry,
        required=(r'OpExtension "SPV_AMD_shader_explicit_vertex_parameter"',),
        counts=((r"\bInterpolateAtVertexAMD\b", 5),),
    )
    try:
        check_pixel_parameter_interface(unique_multi_entry.stdout, "first", "second")
    except ValueError as exc:
        fail("AMD local helper pixel entry interface", unique_multi_entry, str(exc))
    print("PASS AMD local helper pixel entry interface")
    require_failure(
        "Vulkan rejects independently exported HLSL library helper",
        compile_shader(
            "amd.intrinsics.vertex-parameter-shared-helper.hlsl",
            "-T", "lib_6_6", "-E", "main", "-fcgl", "-spirv",
            "-fspv-extension=AMD", "-DUNIQUE_OWNER", "-DAMD_EXPORT_HELPER",
        ),
        "Capability Linkage is not allowed by Vulkan",
    )
    require_failure(
        "AMD raw vertex-parameter non-pixel owner is rejected",
        compile_shader(
            "amd.intrinsics.vertex-parameter-shared-helper.hlsl",
            "-T", "lib_6_6", "-E", "main", "-fcgl", "-spirv",
            "-fspv-extension=AMD", "-DUNIQUE_OWNER", "-DVERTEX_OWNER",
        ),
        "AMD vertex-parameter access is only valid in a pixel shader",
    )
    require_failure(
        "AMD raw vertex-parameter shared helper has ambiguous entry owner",
        compile_shader(
            "amd.intrinsics.vertex-parameter-shared-helper.hlsl",
            "-T", "lib_6_6", "-E", "main", "-fcgl", "-spirv",
            "-fspv-extension=AMD",
        ),
        "AMD vertex-parameter helper requires a unique shader entry point",
    )
    require_success(
        "AMD raw vertex-parameter default argument unused by second entry",
        compile_shader(
            "amd.intrinsics.vertex-parameter-default-argument.hlsl",
            "-T", "lib_6_6", "-E", "main", "-fcgl", "-spirv",
            "-fspv-extension=AMD",
        ),
        required=(r'OpExtension "SPV_AMD_shader_explicit_vertex_parameter"',),
        counts=((r"\bInterpolateAtVertexAMD\b", 4),),
    )
    default_second_owner = compile_shader(
        "amd.intrinsics.vertex-parameter-default-argument.hlsl",
        "-T", "lib_6_6", "-E", "main", "-fcgl", "-spirv",
        "-fspv-extension=AMD", "-DDEFAULT_SINGLE_OWNER",
    )
    require_success(
        "AMD raw vertex-parameter default argument owned by second entry",
        default_second_owner,
        required=(r'OpExtension "SPV_AMD_shader_explicit_vertex_parameter"',),
        counts=((r"\bInterpolateAtVertexAMD\b", 4),),
    )
    try:
        check_pixel_parameter_interface(default_second_owner.stdout, "second", "first")
    except ValueError as exc:
        fail("AMD default argument pixel entry interface", default_second_owner, str(exc))
    print("PASS AMD default argument pixel entry interface")
    require_failure(
        "AMD raw vertex-parameter default argument shared across entries",
        compile_shader(
            "amd.intrinsics.vertex-parameter-default-argument.hlsl",
            "-T", "lib_6_6", "-E", "main", "-fcgl", "-spirv",
            "-fspv-extension=AMD", "-DDEFAULT_USED",
        ),
        "AMD vertex-parameter helper requires a unique shader entry point",
    )
    require_success(
        "SPIR-V pixel-only discard helper with pixel entry first",
        compile_shader(
            "amd.shared-stage-discard.hlsl",
            "-T", "lib_6_6", "-E", "main", "-fcgl", "-spirv",
            "-DPIXEL_ONLY",
        ),
        required=(r'OpEntryPoint Fragment', r'OpEntryPoint Vertex'),
    )
    require_success(
        "SPIR-V pixel-only discard helper with vertex entry first",
        compile_shader(
            "amd.shared-stage-discard.hlsl",
            "-T", "lib_6_6", "-E", "main", "-fcgl", "-spirv",
            "-DPIXEL_ONLY", "-DVERTEX_FIRST",
        ),
        required=(r'OpEntryPoint Fragment', r'OpEntryPoint Vertex'),
    )
    require_failure(
        "SPIR-V fragment discard helper cannot be shared with vertex",
        compile_shader(
            "amd.shared-stage-discard.hlsl",
            "-T", "lib_6_6", "-E", "main", "-fcgl", "-spirv",
        ),
        "discard statement may only be used in pixel shaders",
    )
    require_failure(
        "SPIR-V shared discard rejects vertex-first entry ordering",
        compile_shader(
            "amd.shared-stage-discard.hlsl",
            "-T", "lib_6_6", "-E", "main", "-fcgl", "-spirv",
            "-DVERTEX_FIRST",
        ),
        "discard statement may only be used in pixel shaders",
    )
    require_failure(
        "SPIR-V discard in patch-constant helper shared with pixel",
        compile_shader(
            "amd.shared-stage-discard-patch-constant.hlsl",
            "-T", "lib_6_6", "-E", "main", "-fcgl", "-spirv",
        ),
        "discard statement may only be used in pixel shaders",
    )
    require_failure(
        "SPIR-V discard in patch-constant helper with hull first",
        compile_shader(
            "amd.shared-stage-discard-patch-constant.hlsl",
            "-T", "lib_6_6", "-E", "main", "-fcgl", "-spirv",
            "-DHULL_FIRST",
        ),
        "discard statement may only be used in pixel shaders",
    )
    require_success(
        "SPIR-V discard helper not called from patch constants",
        compile_shader(
            "amd.shared-stage-discard-patch-constant.hlsl",
            "-T", "lib_6_6", "-E", "main", "-fcgl", "-spirv",
            "-DNO_PATCH_CALL",
        ),
        required=(r"OpEntryPoint Fragment", r"OpEntryPoint TessellationControl"),
    )
    require_success(
        "SPIR-V local fragment helper in universal SPIR-V",
        compile_shader(
            "amd.shared-stage-discard-export.hlsl",
            "-T", "lib_6_6", "-E", "main", "-fcgl", "-spirv",
            "-fspv-target-env=universal1.5",
        ),
        required=(r"OpEntryPoint Fragment",),
    )
    require_failure(
        "SPIR-V exported helper is not pixel-only",
        compile_shader(
            "amd.shared-stage-discard-export.hlsl",
            "-T", "lib_6_6", "-E", "main", "-fcgl", "-spirv",
            "-fspv-target-env=universal1.5", "-DEXPORTED",
        ),
        "discard statement may only be used in pixel shaders",
    )
    require_failure(
        "SPIR-V global initializer reaches discard from multiple stages",
        compile_shader(
            "amd.shared-stage-discard-global-init.hlsl",
            "-T", "lib_6_6", "-E", "main", "-fcgl", "-spirv",
        ),
        "discard statement may only be used in pixel shaders",
    )
    require_success(
        "SPIR-V global initializer discard with only pixel callers",
        compile_shader(
            "amd.shared-stage-discard-global-init.hlsl",
            "-T", "lib_6_6", "-E", "main", "-fcgl", "-spirv",
            "-DPIXEL_ONLY",
        ),
        required=(r"OpEntryPoint Fragment",),
    )
    require_success(
        "SPIR-V standalone pixel input attribute-at-vertex",
        compile_shader(
            "amd.shared-stage-attribute-at-vertex.hlsl",
            "-T", "ps_6_6", "-E", "pixelMain", "-fcgl", "-spirv",
        ),
        required=(r'SPV_KHR_fragment_shader_barycentric',),
    )
    require_success(
        "SPIR-V upstream attribute-at-vertex function parameter",
        compile_shader(
            "intrinsics.get-attribute-at-vertex.s.funcParam.hlsl",
            "-T", "ps_6_1", "-E", "main", "-fcgl", "-spirv",
        ),
        required=(r"OpEntryPoint Fragment", r"OpFunctionCall"),
    )
    require_failure(
        "SPIR-V attribute-at-vertex helper cannot be shared with vertex",
        compile_shader(
            "amd.shared-stage-attribute-at-vertex.hlsl",
            "-T", "lib_6_6", "-E", "main", "-fcgl", "-spirv", "-DSHARED",
        ),
        "GetAttributeAtVertex only allowed in pixel shader",
    )
    require_failure(
        "SPIR-V attribute-at-vertex helper rejects vertex-first sharing",
        compile_shader(
            "amd.shared-stage-attribute-at-vertex.hlsl",
            "-T", "lib_6_6", "-E", "main", "-fcgl", "-spirv",
            "-DSHARED", "-DVERTEX_FIRST",
        ),
        "GetAttributeAtVertex only allowed in pixel shader",
    )
    require_success(
        "SPIR-V sizeof expression does not call fragment-only helper",
        compile_shader(
            "amd.shared-stage-discard-unevaluated.hlsl",
            "-T", "lib_6_6", "-E", "main", "-fcgl", "-spirv",
        ),
        required=(r"OpEntryPoint Fragment", r"OpEntryPoint Vertex"),
    )
    require_success(
        "SPIR-V vertex-first sizeof does not call fragment-only helper",
        compile_shader(
            "amd.shared-stage-discard-unevaluated.hlsl",
            "-T", "lib_6_6", "-E", "main", "-fcgl", "-spirv",
            "-DVERTEX_FIRST",
        ),
        required=(r"OpEntryPoint Fragment", r"OpEntryPoint Vertex"),
    )
    require_failure(
        "SPIR-V executing fragment-only helper in vertex remains rejected",
        compile_shader(
            "amd.shared-stage-discard-unevaluated.hlsl",
            "-T", "lib_6_6", "-E", "main", "-fcgl", "-spirv",
            "-DREAL_VERTEX_CALL",
        ),
        "discard statement may only be used in pixel shaders",
    )
    require_failure(
        "SPIR-V vertex-first executing fragment-only helper is rejected",
        compile_shader(
            "amd.shared-stage-discard-unevaluated.hlsl",
            "-T", "lib_6_6", "-E", "main", "-fcgl", "-spirv",
            "-DREAL_VERTEX_CALL", "-DVERTEX_FIRST",
        ),
        "discard statement may only be used in pixel shaders",
    )
    require_failure(
        "AMD raw vertex-parameter register bounds",
        compile_shader(
            "amd.intrinsics.vertex-parameter-out-of-range.hlsl",
            "-T", "ps_6_0", "-E", "main", "-fcgl", "-spirv",
            "-fspv-extension=AMD",
        ),
        "AMD vertex-parameter register component is not backed by a pixel shader input",
    )
    require_failure(
        "AMD raw vertex-parameter immediate enforcement",
        compile_shader(
            "amd.intrinsics.vertex-parameter-index.invalid.hlsl",
            "-T", "ps_6_0", "-E", "main", "-fcgl", "-spirv",
            "-fspv-extension=AMD",
        ),
        "AMD vertex-parameter indices must be compile-time integer constants",
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
    require_failure(
        "strict mode overrides experimental msad4 candidate",
        compile_shader(
            "intrinsics.msad4.amd.hlsl",
            "-T", "vs_6_0", "-E", "main", "-fcgl", "-spirv",
            "-fspv-enable-amd-intrinsics",
            "-fspv-require-native-intrinsics",
        ),
        "msad4 has a native AMD mapping to v_mqsad_u32_u8",
    )
    require_failure(
        "experimental msad4 flag rejected for DXIL",
        compile_shader(
            "intrinsics.msad4.amd.hlsl",
            "-T", "vs_6_0", "-E", "main", "-fcgl",
            "-fspv-enable-amd-intrinsics",
        ),
        "-fspv-enable-amd-intrinsics requires -spirv output",
    )
    require_failure(
        "native-SPIR-V audit flag rejected for DXIL",
        compile_shader(
            "intrinsics.msad4.amd.hlsl",
            "-T", "vs_6_0", "-E", "main", "-fcgl",
            "-fspv-require-native-intrinsics",
        ),
        "-fspv-require-native-intrinsics requires -spirv output",
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
            r"\bOpConstant\b[^\n]*\b17\b",
            r"\bOpConstant\b[^\n]*\b19\b",
            r"\bOpConstant\b[^\n]*\b23\b",
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
