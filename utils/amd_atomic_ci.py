#!/usr/bin/env python3
"""Run the AMD 64-bit atomic SPIR-V resource-family regressions."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path


def run_shader(dxc: str, shader: Path, *flags: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [dxc, *flags, str(shader)],
        text=True,
        encoding="utf-8",
        errors="replace",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def require(name: str, text: str, patterns: tuple[str, ...]) -> None:
    missing = [pattern for pattern in patterns if re.search(pattern, text, re.MULTILINE) is None]
    if missing:
        raise RuntimeError(f"{name}: missing SPIR-V patterns: {', '.join(missing)}")
    print(f"PASS AMD atomics: {name}")


def check_case(
    dxc: str,
    shader: Path,
    name: str,
    flags: tuple[str, ...],
    patterns: tuple[str, ...],
) -> bool:
    proc = run_shader(dxc, shader, *flags)
    if proc.returncode != 0:
        output = (proc.stdout + "\n" + proc.stderr)[-8000:]
        print(f"FAIL {shader.name}: compilation failed\n{output}", file=sys.stderr)
        return False
    try:
        require(name, proc.stdout, patterns)
        return True
    except RuntimeError as error:
        print(f"FAIL {error}", file=sys.stderr)
        return False


def check_raw_u64(
    dxc: str,
    shader: Path,
    flags: tuple[str, ...],
) -> bool:
    # The typed representation must reject a 64-bit atomic rather than
    # silently splitting it across independent 32-bit operations.
    legacy = run_shader(dxc, shader, *flags)
    diagnostic = (
        "64-bit RWByteAddressBuffer atomics require SPV_KHR_untyped_pointers"
    )
    output = legacy.stdout + "\n" + legacy.stderr
    if legacy.returncode == 0 or diagnostic not in output:
        print(
            "FAIL AMD atomics: typed raw-buffer 64-bit atomic contract "
            "did not produce the required diagnostic",
            file=sys.stderr,
        )
        print(output[-8000:], file=sys.stderr)
        return False
    print("PASS AMD atomics: typed raw-buffer 64-bit atomic rejection")

    native = run_shader(
        dxc, shader, *flags, "-fspv-extension=SPV_KHR_untyped_pointers"
    )
    if native.returncode != 0:
        print(
            "FAIL AMD atomics: untyped raw-buffer 64-bit atomic compilation\n"
            + (native.stdout + "\n" + native.stderr)[-8000:],
            file=sys.stderr,
        )
        return False

    spirv = native.stdout
    required = (
        r'OpExtension "SPV_KHR_untyped_pointers"',
        r"\bOpCapability UntypedPointersKHR\b",
        r"\bOpCapability Int64Atomics\b",
        r"\bOpTypeUntypedPointerKHR\b",
        r"\bOpUntypedAccessChainKHR\b",
    )
    try:
        missing = [
            pattern for pattern in required
            if re.search(pattern, spirv, re.MULTILINE) is None
        ]
        if missing:
            raise RuntimeError("missing SPIR-V declarations: " + ", ".join(missing))

        int64_types = set(re.findall(
            r"^\s*(%\S+)\s*=\s*OpTypeInt\s+64\s+[01]\b",
            spirv, re.MULTILINE,
        ))
        untyped_chains = set(re.findall(
            r"^\s*(%\S+)\s*=\s*OpUntypedAccessChainKHR\b",
            spirv, re.MULTILINE,
        ))
        atomic_ops = re.findall(
            r"^\s*%\S+\s*=\s*(OpAtomic\w+)\s+(%\S+)\s+(%\S+)\b",
            spirv, re.MULTILINE,
        )
        expected = {
            "OpAtomicIAdd", "OpAtomicAnd", "OpAtomicOr", "OpAtomicXor",
            "OpAtomicUMin", "OpAtomicUMax", "OpAtomicExchange",
            "OpAtomicCompareExchange",
        }
        if len(atomic_ops) != len(expected) or {
            opcode for opcode, _, _ in atomic_ops
        } != expected:
            raise RuntimeError(
                "expected eight distinct native 64-bit atomic operations; "
                f"found {[opcode for opcode, _, _ in atomic_ops]}"
            )
        if not all(
            result_type in int64_types and pointer in untyped_chains
            for _, result_type, pointer in atomic_ops
        ):
            raise RuntimeError(
                "one or more native atomics lack a 64-bit result type "
                "or an untyped access-chain pointer"
            )
    except RuntimeError as error:
        print(f"FAIL AMD atomics: {error}", file=sys.stderr)
        return False

    print("PASS AMD atomics: eight native 64-bit untyped raw-buffer operations")
    return True

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxc", required=True)
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    tests = root / "tools/clang/test/CodeGenSPIRV"
    dxc = str(Path(args.dxc).resolve())

    common = (
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
    buffer_flags = (
        "-T", "cs_6_6", "-E", "main", "-fcgl", "-spirv",
        "-fspv-target-env=vulkan1.2",
    )
    image_flags = (
        *buffer_flags,
        "-fspv-extension=SPV_EXT_shader_image_int64",
    )

    failed = False
    if not check_case(
        dxc,
        tests / "amd.intrinsics.atomic-u64.hlsl",
        "structured buffer",
        buffer_flags,
        common + (r"\bOpAtomicSMin\b", r"\bOpAtomicSMax\b"),
    ):
        failed = True
    if not check_raw_u64(
        dxc,
        tests / "amd.intrinsics.atomic-u64-byteaddress.hlsl",
        buffer_flags,
    ):
        failed = True
    if not check_case(
        dxc,
        tests / "amd.intrinsics.atomic-u64-image.hlsl",
        "typed image operations",
        image_flags,
        common + (
            r'OpExtension "SPV_EXT_shader_image_int64"',
            r"OpCapability Int64ImageEXT",
        ),
    ):
        failed = True
    if not check_case(
        dxc,
        tests / "amd.intrinsics.atomic-u64-image-shapes.hlsl",
        "typed image 1D/2D/3D",
        image_flags,
        (
            r'OpExtension "SPV_EXT_shader_image_int64"',
            r"OpCapability Int64ImageEXT",
            r"OpCapability Image1D",
            r"\bOpAtomicIAdd\b",
            r"\bOpAtomicUMin\b",
            r"\bOpAtomicCompareExchange\b",
        ),
    ):
        failed = True

    if failed:
        return 1

    print("AMD 64-bit atomic resource-family contracts: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
