#!/usr/bin/env python3
"""Run AMD Vulkan low-level math regressions with only a built dxc executable."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Case:
    name: str
    shader: str
    required: tuple[str, ...] = ()
    forbidden: tuple[str, ...] = ()
    counts: tuple[tuple[str, int], ...] = ()
    flags: tuple[str, ...] = ()
    target: str = "cs_6_2"


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


def fail(case: Case, proc: subprocess.CompletedProcess[str], reason: str) -> None:
    output = (proc.stdout + "\n" + proc.stderr).strip()
    if len(output) > 8000:
        output = output[-8000:]
    raise RuntimeError(
        f"{case.name}: {reason}\nreturn code: {proc.returncode}\n{output}"
    )


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

    cases = (
        Case(
            "exact core math",
            "amd.math.core.hlsl",
            required=(
                r"\bOpBitFieldUExtract\b", r"\bOpBitFieldSExtract\b",
                r"\bOpBitReverse\b", r"\bOpBitCount\b",
                r"\bOpExtInst\b.*\bFMid3AMD\b",
            ),
            counts=(
                (r"\bOpBitFieldUExtract\b", 1),
                (r"\bOpBitFieldSExtract\b", 1),
                (r"\bOpBitReverse\b", 1),
                (r"\bOpBitCount\b", 1),
            ),
            flags=("-fspv-extension=AMD",),
        ),
        Case(
            "APUSR FP/bit semantic surface",
            "amd.math.apusr-fp-bit.hlsl",
            required=(
                r"\bOpExtInst\b.*\bFindILsb\b",
                r"\bOpExtInst\b.*\bFindUMsb\b",
                r"\bOpExtInst\b.*\bFindSMsb\b",
                r"\bOpExtInst\b.*\bPackHalf2x16\b",
                r"\bOpExtInst\b.*\bUnpackHalf2x16\b",
                r"\bOpExtInst\b.*\bLog\b",
                r"\bOpExtInst\b.*\bExp\b",
                r"\bOpExtInst\b.*\bTanh\b",
                r"\bOpExtInst\b.*\bPow\b",
                r"\bOpExtInst\b.*\bFrexpStruct\b",
                r"\bOpExtInst\b.*\bExp2\b",
                r"\bOpFMul\b",
            ),
        ),
        Case(
            "MUL24 canonical structure",
            "amd.math.candidates.hlsl",
            required=(
                r"\bOpIMul\b", r"\bOpBitwiseAnd\b",
                r"\bOpShiftLeftLogical\b", r"\bOpShiftRightArithmetic\b",
            ),
            counts=((r"\bOpIMul\b", 2),),
        ),
        Case(
            "scalar SAD canonical structure",
            "amd.math.sad-candidates.hlsl",
            required=(r"\bOpBitFieldUExtract\b", r"\bOpSelect\b"),
            forbidden=(r"\bSAbs\b",),
        ),
        Case(
            "quad SAD canonical structure",
            "amd.math.qsad-candidates.hlsl",
            required=(
                r"OpTypeInt 64 0", r"\bOpShiftRightLogical\b",
                r"\bOpBitFieldUExtract\b", r"\bOpSelect\b",
                r"\bOpBitwiseOr\b", r"\bOpShiftLeftLogical\b",
            ),
        ),
        Case(
            "integer VALU canonical structure",
            "amd.math.integer-candidates.hlsl",
            required=(
                r"\bOpBitFieldUExtract\b", r"\bOpBitwiseAnd\b",
                r"\bOpBitwiseOr\b", r"\bOpBitwiseXor\b", r"\bOpNot\b",
                r"\bOpShiftRightLogical\b", r"\bOpShiftRightArithmetic\b",
                r"\bOpShiftLeftLogical\b", r"\bOpIAdd\b", r"\bOpIMul\b",
            ),
        ),
        Case(
            "packed integer dot surface",
            "amd.dot.math.hlsl",
            required=(
                r"OpCapability DotProduct",
                r"OpCapability DotProductInput4x8BitPacked",
                r'OpExtension "SPV_KHR_integer_dot_product"',
                r"\bOpSUDot\b", r"\bOpSDotAccSat\b",
                r"\bOpUDotAccSat\b", r"\bOpSUDotAccSat\b",
                r"\bOpBitFieldSExtract\b", r"\bOpBitFieldUExtract\b",
                r"\bOpIMul\b",
            ),
            counts=(
                (r"\bOpSDot\b", 1),
                (r"\bOpUDot\b", 1),
                (r"\bOpSUDot\b", 2),
                (r"\bOpSDotAccSat\b", 1),
                (r"\bOpUDotAccSat\b", 1),
                (r"\bOpSUDotAccSat\b", 2),
            ),
            target="cs_6_4",
        ),
    )

    for case in cases:
        common = ("-T", case.target, "-E", "main", "-fcgl", "-spirv")
        proc = run([dxc, *common, *case.flags, str(tests / case.shader)])
        if proc.returncode != 0:
            fail(case, proc, "compilation failed")
        for pattern in case.required:
            if re.search(pattern, proc.stdout, re.MULTILINE) is None:
                fail(case, proc, f"missing SPIR-V pattern: {pattern}")
        for pattern in case.forbidden:
            if re.search(pattern, proc.stdout, re.MULTILINE) is not None:
                fail(case, proc, f"forbidden SPIR-V pattern: {pattern}")
        for pattern, expected in case.counts:
            actual = len(re.findall(pattern, proc.stdout, re.MULTILINE))
            if actual != expected:
                fail(case, proc, f"pattern {pattern}: got {actual}, expected {expected}")
        print(f"PASS AMD math: {case.name}")

    print("AMD Vulkan low-level math regressions: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"FAIL {error}", file=sys.stderr)
        raise SystemExit(1)
