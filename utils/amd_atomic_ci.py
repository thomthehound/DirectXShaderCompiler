#!/usr/bin/env python3
"""Run the AMD 64-bit atomic SPIR-V resource-family regressions."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path


def compile_shader(dxc: str, shader: Path, *flags: str) -> str:
    proc = subprocess.run(
        [dxc, *flags, str(shader)],
        text=True,
        encoding="utf-8",
        errors="replace",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if proc.returncode != 0:
        output = (proc.stdout + "\n" + proc.stderr)[-8000:]
        raise RuntimeError(f"{shader.name}: compilation failed\n{output}")
    return proc.stdout


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
    try:
        text = compile_shader(dxc, shader, *flags)
        require(name, text, patterns)
        return True
    except RuntimeError as error:
        print(f"FAIL {error}", file=sys.stderr)
        return False


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
    if not check_case(
        dxc,
        tests / "amd.intrinsics.atomic-u64-byteaddress.hlsl",
        "byte-address buffer",
        buffer_flags,
        common,
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
