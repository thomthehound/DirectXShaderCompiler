#!/usr/bin/env python3
"""Check AMD DXC integration contracts against the current generated intrinsic API.

This is a source-level preflight, not a substitute for compiling shaders or
validating SPIR-V. Run it before the expensive Windows compiler build.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
AMD_SUITES = (
    "amd_spirv_ci.py",
    "amd_math_ci.py",
    "amd_subgroup_ci.py",
    "amd_atomic_ci.py",
    "amd_packing_ci.py",
    "amd_bfloat16_ci.py",
    "amd_matrix_ci.py",
    "amd_mixed_dot_ci.py",
    "amd_dxil_ci.py",
)


def check() -> list[str]:
    errors: list[str] = []
    header = (ROOT / "include/dxc/HlslIntrinsicOp.h").read_text(encoding="utf-8")
    definitions = (ROOT / "utils/hct/gen_intrin_main.txt").read_text(encoding="utf-8")
    emitter = (ROOT / "tools/clang/lib/SPIRV/SpirvEmitter.cpp").read_text(
        encoding="utf-8"
    )

    intrinsic_ids = set(re.findall(r"^\s*(IOP_\w+)\s*=", header, re.MULTILINE))
    emitter_ids = set(re.findall(r"\bIntrinsicOp::(IOP_\w+)\b", emitter))
    for name in sorted(emitter_ids - intrinsic_ids):
        errors.append(f"SPIR-V emitter references undefined intrinsic: {name}")

    declared_linalg = set(re.findall(
        r"\b(__builtin_LinAlg_\w+)\s*\(", definitions
    ))
    fixture_dir = ROOT / "tools/clang/test/CodeGenSPIRV"
    fixtures = sorted(fixture_dir.glob("linalg.*.amd.hlsl"))
    if not fixtures:
        errors.append("No AMD linear-algebra fixtures were found")

    for path in fixtures:
        source = path.read_text(encoding="utf-8")
        for intrinsic in sorted(set(re.findall(
            r"(?<![\w:])(__builtin_LinAlg_\w+)\s*\(", source
        ))):
            errors.append(f"{path.relative_to(ROOT)}: unqualified {intrinsic}")
        for intrinsic in sorted(set(re.findall(
            r"\bdx::(__builtin_LinAlg_\w+)\s*\(", source
        ))):
            if intrinsic not in declared_linalg:
                errors.append(f"{path.relative_to(ROOT)}: undeclared {intrinsic}")
            enum_name = "IOP_Dx" + intrinsic
            if enum_name not in intrinsic_ids:
                errors.append(
                    f"{path.relative_to(ROOT)}: missing enum {enum_name}"
                )

    fill_def = next(
        (line for line in definitions.splitlines()
         if "__builtin_LinAlg_FillMatrix(" in line), ""
    )
    if "in bool isInputSigned, in numeric value" not in fill_def:
        errors.append("LinAlg FillMatrix signature changed; audit argument lowering")
    fill_start = emitter.find(
        "case hlsl::IntrinsicOp::IOP_Dx__builtin_LinAlg_FillMatrix:"
    )
    fill_end = emitter.find(
        "case hlsl::IntrinsicOp::IOP_Dx__builtin_LinAlg_CopyConvertMatrix:",
        fill_start,
    )
    if fill_start < 0 or fill_end < 0 or "callExpr->getArg(2)" not in emitter[
        fill_start:fill_end
    ]:
        errors.append("LinAlg FillMatrix value must come from argument 2")

    shapes = fixture_dir / "linalg.cooperative-matrix-shapes.amd.hlsl"
    if shapes.is_file():
        content = shapes.read_text(encoding="utf-8")
        if "not %dxc" not in content or "POINTER: error:" not in content:
            errors.append("Matrix shapes fixture must assert the pointer blocker")

    for name in (*AMD_SUITES, "amd_vulkan_isa_probe/raw_buffer_vector_probe.py"):
        path = ROOT / "utils" / name
        try:
            compile(path.read_text(encoding="utf-8"), str(path), "exec")
        except (OSError, SyntaxError) as exc:
            errors.append(f"{path.relative_to(ROOT)}: {exc}")
    return errors


def main() -> int:
    errors = check()
    if errors:
        for error in errors:
            print("FAIL AMD upstream preflight: " + error, file=sys.stderr)
        return 1
    print("PASS AMD upstream intrinsic and fixture source preflight")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
