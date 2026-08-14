#!/usr/bin/env python3
"""Compile the SAD/MSAD probe corpus and gather AMD native-code evidence.

Proof sources are kept distinct on purpose:
  * SPIR-V shape: what DXC actually emitted.
  * VK_AMD_shader_info: installed Vulkan driver's disassembly.
  * RGA live Vulkan: installed Vulkan driver through RGA.
  * RGA offline: AMD offline compiler for an explicit GFX target.

An offline hit is useful evidence, but is not promoted to installed-driver proof.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Iterable

SAD_ISA_RE = re.compile(
    r"\b(v_(?:sad(?:_hi)?_u(?:8|16|32)|msad_u8|qsad_pk_u16_u8|"
    r"mqsad_(?:pk_u16_u8|u32_u8)))\b",
    re.IGNORECASE,
)
EXACT_MSAD4_ISA = "v_mqsad_u32_u8"


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
    spv: str | None = None
    compile: CommandResult | None = None
    spirv_disassembly: str | None = None
    spirv_summary: dict[str, object] = field(default_factory=dict)
    driver_probe: CommandResult | None = None
    driver_isa: str | None = None
    driver_sad_instructions: list[str] = field(default_factory=list)
    driver_sad_instruction_counts: dict[str, int] = field(default_factory=dict)
    driver_msad4_verdict: str | None = None
    rga_live: CommandResult | None = None
    rga_live_isa: str | None = None
    rga_live_sad_instructions: list[str] = field(default_factory=list)
    rga_live_sad_instruction_counts: dict[str, int] = field(default_factory=dict)
    rga_live_msad4_verdict: str | None = None
    rga_offline: CommandResult | None = None
    rga_offline_isa: str | None = None
    rga_offline_sad_instructions: list[str] = field(default_factory=list)
    rga_offline_sad_instruction_counts: dict[str, int] = field(default_factory=dict)
    rga_offline_msad4_verdict: str | None = None


def quote_cmd(argv: Iterable[str]) -> str:
    return " ".join(shlex.quote(str(x)) for x in argv)


def run(argv: list[str], *, cwd: Path | None = None) -> CommandResult:
    proc = subprocess.run(
        argv,
        cwd=cwd,
        text=True,
        encoding="utf-8",
        errors="replace",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    return CommandResult(argv=argv, returncode=proc.returncode,
                         stdout=proc.stdout, stderr=proc.stderr)


def tool(value: str | None, fallback: str) -> str | None:
    if value:
        return str(Path(value))
    return shutil.which(fallback)


def summarize_spirv(text: str) -> dict[str, object]:
    ops = [
        "OpUDot",
        "OpSDot",
        "OpSUDot",
        "OpIAdd",
        "OpISub",
        "OpSAbs",
        "OpBitFieldUExtract",
        "OpBitwiseAnd",
        "OpShiftRightLogical",
        "OpSelect",
    ]
    capabilities = sorted(set(re.findall(r"^\s*OpCapability\s+(\S+)", text, re.M)))
    extensions = sorted(set(re.findall(r'^\s*OpExtension\s+"([^"]+)"', text, re.M)))
    return {
        "instruction_counts": {op: len(re.findall(rf"\b{re.escape(op)}\b", text)) for op in ops},
        "capabilities": capabilities,
        "extensions": extensions,
    }



def locate_tool_output(requested: Path) -> Path | None:
    """Return requested output or an RGA-prefixed/suffixed variant."""
    if requested.exists():
        return requested
    candidates = list(requested.parent.glob(f"*{requested.stem}*{requested.suffix}"))
    if not candidates:
        return None
    return max(candidates, key=lambda item: item.stat().st_mtime_ns)


def sad_instruction_counts(path: Path) -> dict[str, int]:
    if not path.exists():
        return {}
    text = path.read_text(encoding="utf-8", errors="replace")
    counts: dict[str, int] = {}
    for match in SAD_ISA_RE.finditer(text):
        name = match.group(1).lower()
        counts[name] = counts.get(name, 0) + 1
    return dict(sorted(counts.items()))


def sad_instructions(counts: dict[str, int]) -> list[str]:
    return sorted(counts)


def msad4_isa_verdict(counts: dict[str, int], ran: CommandResult | None) -> str | None:
    if ran is None:
        return None
    if ran.returncode != 0:
        return f"disassembly-failed:{ran.returncode}"
    if counts.get(EXACT_MSAD4_ISA, 0) != 0:
        return "exact-native-v_mqsad_u32_u8"
    if counts:
        return "specialized-sad-family-partial"
    return "no-sad-family-mnemonic"


def result_dict(value: CommandResult | None) -> dict[str, object] | None:
    return asdict(value) if value is not None else None


def case_dict(case: CaseResult) -> dict[str, object]:
    data = asdict(case)
    for key in ("compile", "driver_probe", "rga_live", "rga_offline"):
        data[key] = result_dict(getattr(case, key))
    return data


def write_markdown(path: Path, args: argparse.Namespace, cases: list[CaseResult],
                   strict_result: CommandResult | None) -> None:
    lines: list[str] = []
    lines.append("# AMD SAD/MSAD Vulkan native-lowering probe")
    lines.append("")
    lines.append("This report deliberately keeps compiler proof levels separate.")
    lines.append("")
    lines.append("| Case | SPIR-V | Installed driver | RGA live | RGA offline |")
    lines.append("|---|---|---|---|---|")

    def fmt_instr(counts: dict[str, int], verdict: str | None,
                  ran: CommandResult | None) -> str:
        if ran is None:
            return "not run"
        if ran.returncode != 0:
            return f"failed ({ran.returncode})"
        if not counts:
            return "no SAD-family mnemonic found"
        instr = ", ".join(f"`{name}` x{count}" for name, count in counts.items())
        if verdict == "exact-native-v_mqsad_u32_u8":
            return f"**EXACT MQSAD**: {instr}"
        return f"partial SAD-family: {instr}"

    for case in cases:
        counts = case.spirv_summary.get("instruction_counts", {})
        if counts:
            spv_desc = ", ".join(
                f"{k}={v}" for k, v in counts.items() if isinstance(v, int) and v
            ) or "disassembled"
        elif case.compile and case.compile.returncode == 0:
            spv_desc = "compiled; no spirv-dis"
        else:
            spv_desc = "compile failed"
        lines.append(
            f"| `{case.name}` | {spv_desc} | "
            f"{fmt_instr(case.driver_sad_instruction_counts, case.driver_msad4_verdict, case.driver_probe)} | "
            f"{fmt_instr(case.rga_live_sad_instruction_counts, case.rga_live_msad4_verdict, case.rga_live)} | "
            f"{fmt_instr(case.rga_offline_sad_instruction_counts, case.rga_offline_msad4_verdict, case.rga_offline)} |"
        )

    lines.append("")
    lines.append("## Interpretation")
    lines.append("")
    lines.append("- **Installed driver** is the strongest result here: the probe calls `vkGetShaderInfoAMD` on the pipeline compiled by the selected Vulkan device.")
    lines.append("- **RGA live** is also installed-driver evidence when RGA successfully creates the Vulkan pipeline through that driver.")
    lines.append("- **RGA offline** is target-selectable AMD compiler evidence, but is intentionally not treated as proof of what the installed Windows Radeon driver emits.")
    lines.append("- **Exact `msad4` native proof** means the ISA contains `v_mqsad_u32_u8`, whose four-window masked-SAD shape matches HLSL `msad4`.")
    lines.append("- Other `v_sad*`/`v_msad*`/`v_qsad*`/`v_mqsad*` mnemonics are reported as partial specialized lowering, not silently promoted to the exact one-instruction result.")
    lines.append("- Absence of a SAD-family mnemonic is a negative result only when the relevant compile/disassembly command itself succeeded.")
    lines.append("")

    if strict_result is not None:
        lines.append("## Strict-native contract check")
        lines.append("")
        lines.append(f"Return code: `{strict_result.returncode}`")
        lines.append("")
        if strict_result.returncode == 0:
            lines.append("**FAIL:** strict-native `msad4` unexpectedly compiled successfully; the contract guard did not fire.")
        else:
            lines.append("Strict-native compilation rejected `msad4` as expected.")
        output = (strict_result.stdout + "\n" + strict_result.stderr).strip()
        if output:
            lines.append("")
            lines.append("```text")
            lines.extend(output.splitlines()[-20:])
            lines.append("```")

    lines.append("")
    lines.append("## Commands")
    lines.append("")
    for case in cases:
        for label, result in (
            ("DXC", case.compile),
            ("driver", case.driver_probe),
            ("RGA live", case.rga_live),
            ("RGA offline", case.rga_offline),
        ):
            if result is not None:
                lines.append(f"- {case.name} / {label}: `{quote_cmd(result.argv)}` -> `{result.returncode}`")
    if strict_result is not None:
        lines.append(f"- strict-native: `{quote_cmd(strict_result.argv)}` -> `{strict_result.returncode}`")

    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxc", help="DXC executable containing the AMD SPIR-V fork")
    parser.add_argument("--vanilla-dxc", help="Optional upstream/vanilla DXC for an extra control")
    parser.add_argument("--spirv-dis", dest="spirv_dis", help="spirv-dis executable")
    parser.add_argument("--driver-probe", help="amd_vulkan_isa_probe executable")
    parser.add_argument("--rga", help="RGA CLI executable")
    parser.add_argument("--rga-target", help="GFX target for RGA offline mode, e.g. gfx1151")
    parser.add_argument("--rga-live", action="store_true", help="Also try RGA live Vulkan mode")
    parser.add_argument("--rga-pso", help="Optional .cpso for RGA live mode")
    parser.add_argument("--out-dir", default="out/amd-sad-native-probe")
    parser.add_argument("--target-env", default="vulkan1.2")
    args = parser.parse_args()

    dxc = tool(args.dxc, "dxc")
    if not dxc:
        parser.error("DXC not found; pass --dxc")
    spirv_dis = tool(args.spirv_dis, "spirv-dis")
    driver_probe = tool(args.driver_probe, "amd_vulkan_isa_probe")
    rga = tool(args.rga, "rga") or tool(args.rga, "rga.exe")

    here = Path(__file__).resolve().parent
    shaders = here / "shaders"
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    cases_spec: list[tuple[str, Path, list[str], str]] = [
        ("msad4.builtin.baseline", shaders / "msad4_builtin.hlsl", [], dxc),
        ("msad4.builtin.amd", shaders / "msad4_builtin.hlsl", ["-fspv-enable-amd-intrinsics"], dxc),
        ("msad4.manual", shaders / "msad4_manual.hlsl", [], dxc),
    ]
    if args.vanilla_dxc:
        cases_spec.append(("msad4.builtin.vanilla", shaders / "msad4_builtin.hlsl", [], str(Path(args.vanilla_dxc))))

    cases: list[CaseResult] = []
    common = ["-spirv", "-E", "main", "-T", "cs_6_0", f"-fspv-target-env={args.target_env}"]

    for name, shader, flags, compiler in cases_spec:
        spv = out_dir / f"{name}.spv"
        argv = [compiler, *common, *flags, str(shader), "-Fo", str(spv)]
        compile_result = run(argv)
        case = CaseResult(name=name, shader=str(shader), flags=flags,
                          spv=str(spv), compile=compile_result)
        cases.append(case)
        print(f"[{name}] DXC -> {compile_result.returncode}")
        if compile_result.returncode != 0:
            continue

        if spirv_dis:
            dis_path = out_dir / f"{name}.spvasm"
            dis_result = run([spirv_dis, str(spv), "-o", str(dis_path)])
            if dis_result.returncode == 0 and dis_path.exists():
                text = dis_path.read_text(encoding="utf-8", errors="replace")
                case.spirv_disassembly = str(dis_path)
                case.spirv_summary = summarize_spirv(text)

        if driver_probe:
            isa_path = out_dir / f"{name}.driver.isa"
            probe_result = run([driver_probe, str(spv), str(isa_path)])
            case.driver_probe = probe_result
            if isa_path.exists():
                case.driver_isa = str(isa_path)
                case.driver_sad_instruction_counts = sad_instruction_counts(isa_path)
                case.driver_sad_instructions = sad_instructions(case.driver_sad_instruction_counts)
            case.driver_msad4_verdict = msad4_isa_verdict(
                case.driver_sad_instruction_counts, probe_result)
            print(f"[{name}] installed driver -> {probe_result.returncode}: "
                  f"{case.driver_msad4_verdict} {case.driver_sad_instruction_counts}")

        if rga and args.rga_live:
            isa_path = out_dir / f"{name}.rga-live.isa"
            live_argv = [rga, "-s", "vulkan", "--comp", str(spv), "--isa", str(isa_path)]
            if args.rga_pso:
                live_argv += ["--pso", str(Path(args.rga_pso))]
            live_result = run(live_argv)
            case.rga_live = live_result
            actual_isa = locate_tool_output(isa_path)
            if actual_isa is not None:
                case.rga_live_isa = str(actual_isa)
                case.rga_live_sad_instruction_counts = sad_instruction_counts(actual_isa)
                case.rga_live_sad_instructions = sad_instructions(case.rga_live_sad_instruction_counts)
            case.rga_live_msad4_verdict = msad4_isa_verdict(
                case.rga_live_sad_instruction_counts, live_result)
            print(f"[{name}] RGA live -> {live_result.returncode}: "
                  f"{case.rga_live_msad4_verdict} {case.rga_live_sad_instruction_counts}")

        if rga and args.rga_target:
            isa_path = out_dir / f"{name}.rga-offline.{args.rga_target}.isa"
            offline_argv = [rga, "-s", "vk-spv-offline", "-c", args.rga_target,
                            "--isa", str(isa_path), str(spv)]
            offline_result = run(offline_argv)
            case.rga_offline = offline_result
            actual_isa = locate_tool_output(isa_path)
            if actual_isa is not None:
                case.rga_offline_isa = str(actual_isa)
                case.rga_offline_sad_instruction_counts = sad_instruction_counts(actual_isa)
                case.rga_offline_sad_instructions = sad_instructions(case.rga_offline_sad_instruction_counts)
            case.rga_offline_msad4_verdict = msad4_isa_verdict(
                case.rga_offline_sad_instruction_counts, offline_result)
            print(f"[{name}] RGA offline -> {offline_result.returncode}: "
                  f"{case.rga_offline_msad4_verdict} {case.rga_offline_sad_instruction_counts}")

    strict_spv = out_dir / "msad4.strict-should-not-exist.spv"
    strict_argv = [dxc, *common, "-fspv-enable-amd-intrinsics",
                   "-fspv-require-native-intrinsics",
                   str(shaders / "msad4_builtin.hlsl"), "-Fo", str(strict_spv)]
    strict_result = run(strict_argv)
    print(f"[msad4.strict] expected failure -> {strict_result.returncode}")

    report = {
        "schema": 2,
        "exact_msad4_isa": EXACT_MSAD4_ISA,
        "dxc": dxc,
        "spirv_dis": spirv_dis,
        "driver_probe": driver_probe,
        "rga": rga,
        "rga_target": args.rga_target,
        "rga_live_requested": args.rga_live,
        "cases": [case_dict(case) for case in cases],
        "strict_native": result_dict(strict_result),
    }
    json_path = out_dir / "report.json"
    md_path = out_dir / "REPORT.md"
    json_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    write_markdown(md_path, args, cases, strict_result)
    print(f"report: {md_path}")

    if strict_result.returncode == 0:
        return 5
    if not any(c.compile and c.compile.returncode == 0 for c in cases):
        return 6
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
