#!/usr/bin/env python3
"""Qualify typed vs. untyped ByteAddressBuffer SPIR-V codegen."""

from __future__ import annotations

import argparse
import json
import re
import shlex
import shutil
import subprocess
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Iterable


@dataclass
class CommandResult:
    argv: list[str]
    returncode: int
    stdout: str
    stderr: str


@dataclass
class CaseResult:
    name: str
    shader: str
    flags: list[str]
    compile: CommandResult | None = None
    compile_status: str = "not-run"
    disassemble: CommandResult | None = None
    disassembly_status: str = "not-run"
    spv: str | None = None
    spvasm: str | None = None
    counts: dict[str, int] = field(default_factory=dict)
    facts: dict[str, int | bool] = field(default_factory=dict)
    classification: str = "not-classified"
    notes: list[str] = field(default_factory=list)


OPS = (
    "OpTypeUntypedPointerKHR",
    "OpUntypedVariableKHR",
    "OpUntypedAccessChainKHR",
    "OpUntypedArrayLengthKHR",
    "OpArrayLength",
    "OpAccessChain",
    "OpAtomicIAdd",
    "OpLoad",
    "OpStore",
    "OpCompositeConstruct",
    "OpCompositeExtract",
    "OpBitcast",
)


def quote_cmd(argv: Iterable[str]) -> str:
    return " ".join(shlex.quote(str(x)) for x in argv)


def run(argv: list[str]) -> CommandResult:
    proc = subprocess.run(
        argv,
        text=True,
        encoding="utf-8",
        errors="replace",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    return CommandResult(argv, proc.returncode, proc.stdout, proc.stderr)


def resolve_tool(value: str | None, fallback: str) -> str | None:
    if value:
        candidate = Path(value)
        if candidate.exists():
            return str(candidate.resolve())
        return shutil.which(value)
    return shutil.which(fallback)


def clear_output(path: Path) -> None:
    if path.is_file():
        path.unlink()


def count_ops(text: str) -> dict[str, int]:
    return {op: len(re.findall(rf"\b{op}\b", text)) for op in OPS}


def analyze(text: str) -> tuple[dict[str, int], dict[str, int | bool]]:
    counts = count_ops(text)
    untyped_ids = set(
        re.findall(r"^\s*(%\S+)\s*=\s*OpUntypedAccessChainKHR\b", text, re.MULTILINE)
    )
    atomic_ptrs = re.findall(
        r"^\s*%\S+\s*=\s*OpAtomicIAdd\s+%\S+\s+(%\S+)",
        text,
        re.MULTILINE,
    )
    facts: dict[str, int | bool] = {
        "untyped_extension": 'OpExtension "SPV_KHR_untyped_pointers"' in text,
        "untyped_capability": bool(
            re.search(r"\bOpCapability\s+UntypedPointersKHR\b", text)
        ),
        "storage_buffer_vars_with_data_type": len(
            re.findall(
                r"OpUntypedVariableKHR\s+%\S+\s+StorageBuffer\s+%\S+",
                text,
            )
        ),
        "aligned4_loads": len(re.findall(r"\bOpLoad\b[^\n]*\bAligned\s+4\b", text)),
        "aligned4_stores": len(re.findall(r"\bOpStore\b[^\n]*\bAligned\s+4\b", text)),
        "atomic_uses_untyped_pointer": bool(atomic_ptrs)
        and all(ptr in untyped_ids for ptr in atomic_ptrs),
    }
    return counts, facts


def classify_vector(
    counts: dict[str, int], facts: dict[str, int | bool]
) -> tuple[str, list[str]]:
    notes: list[str] = []
    untyped_vars = counts["OpUntypedVariableKHR"]
    untyped_access = counts["OpUntypedAccessChainKHR"]
    loads = counts["OpLoad"]
    stores = counts["OpStore"]
    constructs = counts["OpCompositeConstruct"]
    extracts = counts["OpCompositeExtract"]

    if (
        facts["untyped_extension"]
        and facts["untyped_capability"]
        and untyped_vars >= 2
        and int(facts["storage_buffer_vars_with_data_type"]) >= 2
        and untyped_access >= 4
        and loads == 2
        and stores == 2
        and int(facts["aligned4_loads"]) == 2
        and int(facts["aligned4_stores"]) == 2
        and constructs == 0
        and extracts == 0
    ):
        return "untyped-vectorized", notes

    if (
        not facts["untyped_extension"]
        and not facts["untyped_capability"]
        and untyped_vars == 0
        and untyped_access == 0
        and loads >= 8
        and stores >= 8
    ):
        return "typed-scalarized", notes

    if untyped_vars or untyped_access:
        notes.append("untyped codegen is present but the complete vector-memory contract failed")
        return "untyped-partial", notes

    notes.append("codegen matches neither the legacy scalar baseline nor the untyped target")
    return "other", notes


def classify_surface(
    counts: dict[str, int], facts: dict[str, int | bool]
) -> tuple[str, list[str]]:
    notes: list[str] = []
    if (
        facts["untyped_extension"]
        and facts["untyped_capability"]
        and counts["OpUntypedVariableKHR"] >= 2
        and int(facts["storage_buffer_vars_with_data_type"]) >= 2
        and counts["OpUntypedAccessChainKHR"] >= 2
        and counts["OpUntypedArrayLengthKHR"] >= 1
        and counts["OpArrayLength"] == 0
        and counts["OpAtomicIAdd"] >= 1
        and facts["atomic_uses_untyped_pointer"]
    ):
        return "untyped-surface-complete", notes

    if (
        not facts["untyped_extension"]
        and not facts["untyped_capability"]
        and counts["OpUntypedVariableKHR"] == 0
        and counts["OpUntypedAccessChainKHR"] == 0
        and counts["OpUntypedArrayLengthKHR"] == 0
        and counts["OpArrayLength"] >= 1
        and counts["OpAtomicIAdd"] >= 1
    ):
        return "typed-surface-compatible", notes

    if counts["OpUntypedVariableKHR"] or counts["OpUntypedAccessChainKHR"]:
        notes.append("untyped resource codegen is present but GetDimensions/atomic coverage is incomplete")
        return "untyped-surface-partial", notes

    notes.append("surface codegen matches neither compatibility nor untyped target shape")
    return "other", notes


def classify(
    kind: str, counts: dict[str, int], facts: dict[str, int | bool]
) -> tuple[str, list[str]]:
    if kind == "vector":
        return classify_vector(counts, facts)
    return classify_surface(counts, facts)


def case_dict(case: CaseResult) -> dict[str, object]:
    return asdict(case)


def run_self_test() -> int:
    legacy_vector = """
OpExtension "SPV_KHR_integer_dot_product"
%p0 = OpAccessChain %ptr %input %zero %idx
%a0 = OpLoad %uint %p0
%p1 = OpAccessChain %ptr %input %zero %idx
%a1 = OpLoad %uint %p1
%p2 = OpAccessChain %ptr %input %zero %idx
%a2 = OpLoad %uint %p2
%p3 = OpAccessChain %ptr %input %zero %idx
%a3 = OpLoad %uint %p3
%v0 = OpCompositeConstruct %v4uint %a0 %a1 %a2 %a3
%p4 = OpAccessChain %ptr %input %zero %idx
%b0 = OpLoad %uint %p4
%p5 = OpAccessChain %ptr %input %zero %idx
%b1 = OpLoad %uint %p5
%p6 = OpAccessChain %ptr %input %zero %idx
%b2 = OpLoad %uint %p6
%p7 = OpAccessChain %ptr %input %zero %idx
%b3 = OpLoad %uint %p7
%v1 = OpCompositeConstruct %v4uint %b0 %b1 %b2 %b3
%q0 = OpCompositeExtract %uint %v0 0
OpStore %p0 %q0
%q1 = OpCompositeExtract %uint %v0 1
OpStore %p1 %q1
%q2 = OpCompositeExtract %uint %v0 2
OpStore %p2 %q2
%q3 = OpCompositeExtract %uint %v0 3
OpStore %p3 %q3
%q4 = OpCompositeExtract %uint %v1 0
OpStore %p4 %q4
%q5 = OpCompositeExtract %uint %v1 1
OpStore %p5 %q5
%q6 = OpCompositeExtract %uint %v1 2
OpStore %p6 %q6
%q7 = OpCompositeExtract %uint %v1 3
OpStore %p7 %q7
"""
    untyped_vector = """
OpCapability UntypedPointersKHR
OpExtension "SPV_KHR_untyped_pointers"
%input = OpUntypedVariableKHR %uptr StorageBuffer %RawBlock
%output = OpUntypedVariableKHR %uptr StorageBuffer %RawBlock
%p0 = OpUntypedAccessChainKHR %uptrv4 %RawBlock %input %zero %idx
%a = OpLoad %v4uint %p0 Aligned 4
%p1 = OpUntypedAccessChainKHR %uptrv4 %RawBlock %input %zero %idx
%b = OpLoad %v4float %p1 Aligned 4
%p2 = OpUntypedAccessChainKHR %uptrv4 %RawBlock %output %zero %idx
OpStore %p2 %a Aligned 4
%p3 = OpUntypedAccessChainKHR %uptrv4 %RawBlock %output %zero %idx
OpStore %p3 %b Aligned 4
"""
    legacy_surface = """
%len = OpArrayLength %uint %input 0
%ptr = OpAccessChain %uptr %output %zero %idx
%old = OpAtomicIAdd %uint %ptr %scope %sem %len
"""
    untyped_surface = """
OpCapability UntypedPointersKHR
OpExtension "SPV_KHR_untyped_pointers"
%input = OpUntypedVariableKHR %uptr StorageBuffer %RawBlock
%output = OpUntypedVariableKHR %uptr StorageBuffer %RawBlock
%len = OpUntypedArrayLengthKHR %uint %RawBlock %input 0
%ptr = OpUntypedAccessChainKHR %uptr32 %RawBlock %output %zero %idx
%old = OpAtomicIAdd %uint %ptr %scope %sem %len
%store = OpUntypedAccessChainKHR %uptr32 %RawBlock %output %zero %idx2
OpStore %store %old Aligned 4
"""
    fixtures = (
        ("vector", legacy_vector, "typed-scalarized"),
        ("vector", untyped_vector, "untyped-vectorized"),
        ("surface", legacy_surface, "typed-surface-compatible"),
        ("surface", untyped_surface, "untyped-surface-complete"),
    )
    for kind, text, expected in fixtures:
        counts, facts = analyze(text)
        actual, notes = classify(kind, counts, facts)
        if actual != expected:
            print(f"self-test failed: {kind}: expected {expected}, got {actual}: {notes}")
            return 1
    print("raw-buffer SPIR-V contract parser self-test: PASS")
    return 0


def write_markdown(
    path: Path,
    cases: list[CaseResult],
    shader_profile: str,
    target_env: str,
    contract_ready: bool,
) -> None:
    lines = [
        "# Raw ByteAddressBuffer untyped-pointer contract",
        "",
        f"Shader profile: `{shader_profile}`. SPIR-V target: `{target_env}`.",
        f"Full untyped contract ready: `{contract_ready}`.",
        "",
        "The vector shader performs exactly two 16-byte loads and two 16-byte stores",
        "at byte offsets that are 4-byte aligned but intentionally not 16-byte aligned.",
        "The surface shader independently covers resource Data Type operands,",
        "`GetDimensions`, and an existing 32-bit raw atomic.",
        "",
        "| Case | Classification | Loads | Stores | Untyped vars | Untyped access | Untyped length | Atomic add |",
        "|---|---|---:|---:|---:|---:|---:|---:|",
    ]
    for case in cases:
        c = case.counts
        lines.append(
            f"| `{case.name}` | `{case.classification}` | {c.get('OpLoad', 0)} | "
            f"{c.get('OpStore', 0)} | {c.get('OpUntypedVariableKHR', 0)} | "
            f"{c.get('OpUntypedAccessChainKHR', 0)} | "
            f"{c.get('OpUntypedArrayLengthKHR', 0)} | {c.get('OpAtomicIAdd', 0)} |"
        )
    lines += [
        "",
        "## Contract",
        "",
        "- Restricted legacy cases forbid `SPV_KHR_untyped_pointers` and must retain typed compatibility codegen.",
        "- Untyped cases require the extension and `UntypedPointersKHR` capability.",
        "- StorageBuffer `OpUntypedVariableKHR` instructions must carry a concrete Data Type operand.",
        "- The vector case requires exactly two aligned-4 vector loads and two aligned-4 vector stores with no composite reconstruction/extraction.",
        "- `GetDimensions` must use `OpUntypedArrayLengthKHR` on an untyped raw buffer.",
        "- Existing 32-bit `InterlockedAdd` must receive a pointer produced by `OpUntypedAccessChainKHR`.",
        "",
        "## Notes",
        "",
    ]
    for case in cases:
        notes = "; ".join(case.notes) if case.notes else "none"
        lines.append(f"- `{case.name}`: {notes}")
    lines += ["", "## Commands", ""]
    for case in cases:
        if case.compile is not None:
            lines.append(f"- `{case.name}` / DXC: `{quote_cmd(case.compile.argv)}` -> `{case.compile.returncode}`")
        if case.disassemble is not None:
            lines.append(f"- `{case.name}` / spirv-dis: `{quote_cmd(case.disassemble.argv)}` -> `{case.disassemble.returncode}`")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxc", help="DXC executable to qualify")
    parser.add_argument("--spirv-dis", dest="spirv_dis", help="spirv-dis executable")
    parser.add_argument("--target-env", default="vulkan1.2")
    parser.add_argument("--shader-profile", default="cs_6_6")
    parser.add_argument("--out-dir", default="out/raw-buffer-vector-probe")
    parser.add_argument("--self-test", action="store_true", help="Test the SPIR-V contract parser without DXC")
    parser.add_argument(
        "--require-untyped-vectorization",
        action="store_true",
        help="Return nonzero unless the untyped vector case reaches the target shape",
    )
    parser.add_argument(
        "--require-untyped-contract",
        action="store_true",
        help="Return nonzero unless vectorization, Data Type, GetDimensions, and atomic-pointer contracts all pass",
    )
    args = parser.parse_args()

    if args.self_test:
        return run_self_test()

    dxc = resolve_tool(args.dxc, "dxc")
    if not dxc:
        parser.error("DXC not found; pass --dxc")
    spirv_dis = resolve_tool(args.spirv_dis, "spirv-dis")
    if not spirv_dis:
        parser.error("spirv-dis not found; pass --spirv-dis")

    here = Path(__file__).resolve().parent
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    extension_flags = {
        "legacy": ["-fspv-extension=SPV_KHR_integer_dot_product"],
        "untyped": ["-fspv-extension=SPV_KHR_untyped_pointers"],
    }
    specs = (
        ("legacy-restricted", "vector", "shaders/raw_buffer_vector.hlsl", extension_flags["legacy"]),
        ("untyped-enabled", "vector", "shaders/raw_buffer_vector.hlsl", extension_flags["untyped"]),
        ("legacy-surface", "surface", "shaders/raw_buffer_surface.hlsl", extension_flags["legacy"]),
        ("untyped-surface", "surface", "shaders/raw_buffer_surface.hlsl", extension_flags["untyped"]),
    )
    common = [
        "-spirv", "-E", "main", "-T", args.shader_profile,
        f"-fspv-target-env={args.target_env}",
    ]

    cases: list[CaseResult] = []
    failed = False
    for name, kind, shader_rel, flags in specs:
        shader = here / shader_rel
        case = CaseResult(name=name, shader=shader_rel, flags=flags)
        cases.append(case)
        spv_path = out_dir / f"{name}.spv"
        asm_path = out_dir / f"{name}.spvasm"
        clear_output(spv_path)
        clear_output(asm_path)

        result = run([dxc, *common, *flags, str(shader), "-Fo", str(spv_path)])
        case.compile = result
        case.spv = str(spv_path)
        if result.returncode != 0:
            case.compile_status = "failed"
            print(f"[{name}] DXC -> {result.returncode}")
            print(result.stderr, end="")
            failed = True
            continue
        if not spv_path.exists():
            case.compile_status = "missing-output"
            print(f"[{name}] DXC returned 0 but produced no SPIR-V")
            failed = True
            continue
        case.compile_status = "ok"

        dis = run([spirv_dis, str(spv_path), "-o", str(asm_path)])
        case.disassemble = dis
        case.spvasm = str(asm_path)
        if dis.returncode != 0:
            case.disassembly_status = "failed"
            print(dis.stderr, end="")
            failed = True
            continue
        if not asm_path.exists():
            case.disassembly_status = "missing-output"
            print(f"[{name}] spirv-dis returned 0 but produced no output")
            failed = True
            continue
        case.disassembly_status = "ok"

        text = asm_path.read_text(encoding="utf-8", errors="replace")
        case.counts, case.facts = analyze(text)
        case.classification, case.notes = classify(kind, case.counts, case.facts)
        print(f"[{name}] {case.classification}: {case.counts} {case.facts}")

    by_name = {case.name: case for case in cases}
    vector_ready = by_name["untyped-enabled"].classification == "untyped-vectorized"
    surface_ready = by_name["untyped-surface"].classification == "untyped-surface-complete"
    legacy_ready = (
        by_name["legacy-restricted"].classification == "typed-scalarized"
        and by_name["legacy-surface"].classification == "typed-surface-compatible"
    )
    contract_ready = vector_ready and surface_ready and legacy_ready

    if args.require_untyped_vectorization and not vector_ready:
        failed = True
    if args.require_untyped_contract and not contract_ready:
        failed = True

    report = {
        "schema": 2,
        "dxc": dxc,
        "spirv_dis": spirv_dis,
        "shader_profile": args.shader_profile,
        "target_env": args.target_env,
        "legacy_compatibility_ready": legacy_ready,
        "untyped_vectorization_ready": vector_ready,
        "untyped_surface_ready": surface_ready,
        "untyped_raw_buffer_contract_ready": contract_ready,
        "cases": [case_dict(case) for case in cases],
    }
    (out_dir / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    write_markdown(out_dir / "REPORT.md", cases, args.shader_profile, args.target_env, contract_ready)
    print(f"legacy compatibility ready: {legacy_ready}")
    print(f"untyped vectorization ready: {vector_ready}")
    print(f"untyped surface ready: {surface_ready}")
    print(f"untyped raw-buffer contract ready: {contract_ready}")
    print(f"report: {out_dir / 'REPORT.md'}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
