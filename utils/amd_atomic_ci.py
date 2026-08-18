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

    structured = compile_shader(
        dxc, tests / "amd.intrinsics.atomic-u64.hlsl",
        "-T", "cs_6_6", "-E", "main", "-fcgl", "-spirv",
        "-fspv-target-env=vulkan1.2",
    )
    require("structured buffer", structured, common + (r"\bOpAtomicSMin\b", r"\bOpAtomicSMax\b"))

    byteaddress = compile_shader(
        dxc, tests / "amd.intrinsics.atomic-u64-byteaddress.hlsl",
        "-T", "cs_6_6", "-E", "main", "-fcgl", "-spirv",
        "-fspv-target-env=vulkan1.2",
    )
    require("byte-address buffer", byteaddress, common)

    image_flags = (
        "-T", "cs_6_6", "-E", "main", "-fcgl", "-spirv",
        "-fspv-target-env=vulkan1.2",
        "-fspv-extension=SPV_EXT_shader_image_int64",
    )
    image = compile_shader(dxc, tests / "amd.intrinsics.atomic-u64-image.hlsl", *image_flags)
    require(
        "typed image operations",
        image,
        common + (
            r'OpExtension "SPV_EXT_shader_image_int64"',
            r"OpCapability Int64ImageEXT",
        ),
    )

    shapes = compile_shader(dxc, tests / "amd.intrinsics.atomic-u64-image-shapes.hlsl", *image_flags)
    require(
        "typed image 1D/2D/3D",
        shapes,
        (
            r'OpExtension "SPV_EXT_shader_image_int64"',
            r"OpCapability Int64ImageEXT",
            r"OpCapability Image1D",
            r"\bOpAtomicIAdd\b",
            r"\bOpAtomicUMin\b",
            r"\bOpAtomicCompareExchange\b",
        ),
    )

    print("AMD 64-bit atomic resource-family contracts: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"FAIL {error}", file=sys.stderr)
        raise SystemExit(1)
