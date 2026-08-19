#!/usr/bin/env python3
"""Compare typed and untyped ByteAddressBuffer vector-memory codegen."""

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
    flags: list[str]
    compile: CommandResult | None = None
    compile_status: str = "not-run"
    disassemble: CommandResult | None = None
    disassembly_status: str = "not-run"
    spv: str | None = None
    spvasm: str | None = None
    counts: dict[str, int] = field(default_factory=dict)
    classification: str = "not-classified"
    notes: list[str] = field(default_factory=list)


OPS = (
    "OpTypeUntypedPointerKHR",
    "OpUntypedVariableKHR",
    "OpUntypedAccessChainKHR",
    "OpAccessChain",
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
    return CommandResult(
        argv=argv,
        returncode=proc.returncode,
        stdout=proc.stdout,
        stderr=proc.stderr,
    )


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


def classify(counts: dict[str, int]) -> tuple[str, list[str]]:
    notes: list[str] = []
    untyped_vars = counts.get("OpUntypedVariableKHR", 0)
    untyped_access = counts.get("OpUntypedAccessChainKHR", 0)
    loads = counts.get("OpLoad", 0)
    stores = counts.get("OpStore", 0)
    constructs = counts.get("OpCompositeConstruct", 0)
    extracts = counts.get("OpCompositeExtract", 0)

    if (
        untyped_vars >= 2
        and untyped_access >= 4
        and loads == 2
        and stores == 2
        and constructs == 0
        and extracts == 0
    ):
        return "untyped-vectorized", notes

    if (
        untyped_vars == 0
        and untyped_access == 0
        and loads >= 8
        and stores >= 8
    ):
        if constructs < 2:
            notes.append(
                f"scalar load count is present but only {constructs} composite constructs"
            )
        if extracts < 8:
            notes.append(
                f"scalar store count is present but only {extracts} composite extracts"
            )
        return "typed-scalarized", notes

    if untyped_vars or untyped_access:
        notes.append(
            "untyped-pointer codegen is present but the two vector loads/two vector "
            "stores did not remain as four vector memory operations"
        )
        return "untyped-partial", notes

    notes.append(
        "codegen does not match either the known scalarized baseline or the target "
        "untyped vector-memory shape"
    )
    return "other", notes


def case_dict(case: CaseResult) -> dict[str, object]:
    return asdict(case)


def write_markdown(
    path: Path,
    cases: list[CaseResult],
    shader_profile: str,
    target_env: str,
) -> None:
    by_name = {case.name: case for case in cases}
    legacy = by_name.get("legacy-restricted")
    untyped = by_name.get("untyped-enabled")

    lines = [
        "# Raw ByteAddressBuffer vector-memory comparison",
        "",
        f"Shader profile: `{shader_profile}`. SPIR-V target: `{target_env}`.",
        "",
        "The shader performs exactly two 16-byte loads and two 16-byte stores:",
        "classic `Load4`/`Store4` plus templated `Load<float4>`/vector `Store`.",
        "The byte offsets are 4-byte aligned but intentionally not 16-byte aligned.",
        "",
        "| Case | Classification | Loads | Stores | Untyped vars | Untyped access chains | Constructs | Extracts |",
        "|---|---|---:|---:|---:|---:|---:|---:|",
    ]
    for case in cases:
        c = case.counts
        lines.append(
            f"| `{case.name}` | `{case.classification}` | "
            f"{c.get('OpLoad', 0)} | {c.get('OpStore', 0)} | "
            f"{c.get('OpUntypedVariableKHR', 0)} | "
            f"{c.get('OpUntypedAccessChainKHR', 0)} | "
            f"{c.get('OpCompositeConstruct', 0)} | "
            f"{c.get('OpCompositeExtract', 0)} |"
        )

    if legacy and untyped and legacy.counts and untyped.counts:
        lines += [
            "",
            "## Delta",
            "",
            f"- Load instructions: {legacy.counts.get('OpLoad', 0)} -> {untyped.counts.get('OpLoad', 0)}",
            f"- Store instructions: {legacy.counts.get('OpStore', 0)} -> {untyped.counts.get('OpStore', 0)}",
            f"- Composite constructs: {legacy.counts.get('OpCompositeConstruct', 0)} -> {untyped.counts.get('OpCompositeConstruct', 0)}",
            f"- Composite extracts: {legacy.counts.get('OpCompositeExtract', 0)} -> {untyped.counts.get('OpCompositeExtract', 0)}",
        ]

    lines += [
        "",
        "## Contract",
        "",
        "- `legacy-restricted` explicitly allow-lists an unrelated KHR extension, so `SPV_KHR_untyped_pointers` is forbidden. It must continue to compile through the existing typed compatibility path.",
        "- `untyped-enabled` explicitly permits `SPV_KHR_untyped_pointers`. The target is two `OpUntypedVariableKHR` resources, untyped access chains, two vector `OpLoad`s, and two vector `OpStore`s.",
        "- The optimized case must not depend on 16-byte HLSL address alignment; the probe offsets are only 4-byte aligned.",
        "- Converting pointers after DXC has scalarized the operations is insufficient: the vector memory operations must survive DXC codegen itself.",
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
            lines.append(
                f"- `{case.name}` / DXC: `{quote_cmd(case.compile.argv)}` -> "
                f"`{case.compile.returncode}`"
            )
        if case.disassemble is not None:
            lines.append(
                f"- `{case.name}` / spirv-dis: "
                f"`{quote_cmd(case.disassemble.argv)}` -> "
                f"`{case.disassemble.returncode}`"
            )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxc", help="DXC executable to qualify")
    parser.add_argument("--spirv-dis", dest="spirv_dis", help="spirv-dis executable")
    parser.add_argument("--target-env", default="vulkan1.2")
    parser.add_argument("--shader-profile", default="cs_6_6")
    parser.add_argument("--out-dir", default="out/raw-buffer-vector-probe")
    parser.add_argument(
        "--require-untyped-vectorization",
        action="store_true",
        help="Return nonzero unless the untyped-enabled case reaches the target shape",
    )
    args = parser.parse_args()

    dxc = resolve_tool(args.dxc, "dxc")
    if not dxc:
        parser.error("DXC not found; pass --dxc")
    spirv_dis = resolve_tool(args.spirv_dis, "spirv-dis")
    if not spirv_dis:
        parser.error("spirv-dis not found; pass --spirv-dis")

    here = Path(__file__).resolve().parent
    shader = here / "shaders" / "raw_buffer_vector.hlsl"
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    specs = (
        (
            "legacy-restricted",
            ["-fspv-extension=SPV_KHR_integer_dot_product"],
        ),
        (
            "untyped-enabled",
            ["-fspv-extension=SPV_KHR_untyped_pointers"],
        ),
    )
    common = [
        "-spirv",
        "-E",
        "main",
        "-T",
        args.shader_profile,
        f"-fspv-target-env={args.target_env}",
    ]

    cases: list[CaseResult] = []
    failed = False
    for name, flags in specs:
        case = CaseResult(name=name, flags=flags)
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
        case.counts = count_ops(
            asm_path.read_text(encoding="utf-8", errors="replace")
        )
        case.classification, case.notes = classify(case.counts)
        print(f"[{name}] {case.classification}: {case.counts}")

    by_name = {case.name: case for case in cases}
    legacy = by_name["legacy-restricted"]
    untyped = by_name["untyped-enabled"]
    if (
        legacy.disassembly_status == "ok"
        and legacy.classification != "typed-scalarized"
    ):
        legacy.notes.append(
            "legacy compatibility shape changed while untyped pointers were forbidden"
        )

    requirement_met = untyped.classification == "untyped-vectorized"
    if args.require_untyped_vectorization and not requirement_met:
        failed = True

    report = {
        "schema": 1,
        "dxc": dxc,
        "spirv_dis": spirv_dis,
        "shader_profile": args.shader_profile,
        "target_env": args.target_env,
        "untyped_vectorization_ready": requirement_met,
        "cases": [case_dict(case) for case in cases],
    }
    (out_dir / "report.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    write_markdown(
        out_dir / "REPORT.md",
        cases,
        args.shader_profile,
        args.target_env,
    )
    print(f"untyped vectorization ready: {requirement_met}")
    print(f"report: {out_dir / 'REPORT.md'}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
