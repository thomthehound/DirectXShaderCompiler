#!/usr/bin/env python3
"""Compare APUSR's accumulated msad4 and Vulkan SWAR+UDOT QSad shapes.

The probe keeps evidence levels separate:
* DXC/SPIR-V shape;
* installed Vulkan driver ISA via amd_vulkan_isa_probe;
* optional RGA live-driver ISA;
* optional RGA offline target ISA.

A missing SAD-family mnemonic is a useful negative result, not a probe failure,
provided compilation/disassembly itself succeeded.
"""

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

SAD_ISA_RE = re.compile(
    r"\b(v_(?:sad(?:_hi)?_u(?:8|16|32)|msad_u8|qsad_pk_u16_u8|"
    r"mqsad_(?:pk_u16_u8|u32_u8)))\b",
    re.IGNORECASE,
)


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
    spv: str | None = None
    spirv_disassembly: str | None = None
    spirv_counts: dict[str, int] = field(default_factory=dict)
    driver_probe: CommandResult | None = None
    driver_isa: str | None = None
    driver_sad_counts: dict[str, int] = field(default_factory=dict)
    rga_live: CommandResult | None = None
    rga_live_isa: str | None = None
    rga_live_sad_counts: dict[str, int] = field(default_factory=dict)
    rga_offline: CommandResult | None = None
    rga_offline_isa: str | None = None
    rga_offline_sad_counts: dict[str, int] = field(default_factory=dict)


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
    return CommandResult(argv=argv, returncode=proc.returncode,
                         stdout=proc.stdout, stderr=proc.stderr)


def resolve_tool(value: str | None, fallback: str) -> str | None:
    if value:
        return str(Path(value).resolve())
    return shutil.which(fallback)


def spirv_counts(text: str) -> dict[str, int]:
    ops = (
        "OpUDot", "OpSDot", "OpSUDot", "OpIAdd", "OpISub", "OpSAbs",
        "OpBitwiseAnd", "OpBitwiseOr", "OpBitwiseXor", "OpShiftRightLogical",
        "OpShiftLeftLogical", "OpSelect",
    )
    return {op: len(re.findall(rf"\b{op}\b", text)) for op in ops}


def sad_counts(path: Path) -> dict[str, int]:
    if not path.exists():
        return {}
    text = path.read_text(encoding="utf-8", errors="replace")
    counts: dict[str, int] = {}
    for match in SAD_ISA_RE.finditer(text):
        name = match.group(1).lower()
        counts[name] = counts.get(name, 0) + 1
    return dict(sorted(counts.items()))


def locate_output(requested: Path) -> Path | None:
    if requested.exists():
        return requested
    matches = list(requested.parent.glob(f"*{requested.stem}*{requested.suffix}"))
    return max(matches, key=lambda p: p.stat().st_mtime_ns) if matches else None


def command_dict(value: CommandResult | None) -> dict[str, object] | None:
    return asdict(value) if value is not None else None


def case_dict(value: CaseResult) -> dict[str, object]:
    result = asdict(value)
    for field_name in ("compile", "driver_probe", "rga_live", "rga_offline"):
        result[field_name] = command_dict(getattr(value, field_name))
    return result


def fmt_counts(counts: dict[str, int]) -> str:
    if not counts:
        return "none"
    return ", ".join(f"{name} x{count}" for name, count in counts.items())


def write_markdown(path: Path, cases: list[CaseResult]) -> None:
    lines = [
        "# APUSR QSad native-lowering comparison",
        "",
        "The two shaders model the same 8-row x 2-QSad optical-flow search shape:",
        "the intended accumulated `msad4` path and APUSR's current SWAR+packed-UDOT Vulkan path.",
        "",
        "| Case | SPIR-V nonzero ops | Installed driver SAD-family | RGA live | RGA offline |",
        "|---|---|---|---|---|",
    ]
    for case in cases:
        spv = ", ".join(f"{k}={v}" for k, v in case.spirv_counts.items() if v) or "no disassembly"
        lines.append(
            f"| `{case.name}` | {spv} | {fmt_counts(case.driver_sad_counts)} | "
            f"{fmt_counts(case.rga_live_sad_counts)} | {fmt_counts(case.rga_offline_sad_counts)} |"
        )

    lines += [
        "",
        "## Interpretation",
        "",
        "- Installed-driver ISA is the result that decides whether the current Radeon stack recovers `v_msad*`/`v_mqsad*` from either SPIR-V shape.",
        "- RGA offline is useful target evidence but is not substituted for installed-driver proof.",
        "- No SAD-family mnemonic is a valid negative result when the corresponding disassembly command succeeds.",
        "- Compare instruction counts as well as mnemonic presence: APUSR executes this search shape at high multiplicity, so partial recovery can still leave substantial integer work.",
        "",
        "## Commands",
        "",
    ]
    for case in cases:
        for label, result in (
            ("DXC", case.compile),
            ("installed driver", case.driver_probe),
            ("RGA live", case.rga_live),
            ("RGA offline", case.rga_offline),
        ):
            if result is not None:
                lines.append(f"- `{case.name}` / {label}: `{quote_cmd(result.argv)}` -> `{result.returncode}`")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxc", help="DXC executable containing the AMD SPIR-V work")
    parser.add_argument("--spirv-dis", dest="spirv_dis", help="spirv-dis executable")
    parser.add_argument("--driver-probe", help="amd_vulkan_isa_probe executable")
    parser.add_argument("--rga", help="RGA CLI executable")
    parser.add_argument("--rga-target", help="Offline RGA target, e.g. gfx1151")
    parser.add_argument("--rga-live", action="store_true")
    parser.add_argument("--rga-pso", help="Optional .cpso for RGA live mode")
    parser.add_argument("--target-env", default="vulkan1.2")
    parser.add_argument("--out-dir", default="out/apusr-qsad-probe")
    args = parser.parse_args()

    dxc = resolve_tool(args.dxc, "dxc")
    if not dxc:
        parser.error("DXC not found; pass --dxc")
    spirv_dis = resolve_tool(args.spirv_dis, "spirv-dis")
    driver_probe = resolve_tool(args.driver_probe, "amd_vulkan_isa_probe")
    rga = resolve_tool(args.rga, "rga") or resolve_tool(args.rga, "rga.exe")

    here = Path(__file__).resolve().parent
    shaders = here / "shaders"
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    specs = (
        ("apusr.qsad.msad4", shaders / "apusr_qsad_msad4.hlsl",
         ["-fspv-enable-amd-intrinsics"]),
        ("apusr.qsad.swar-udot", shaders / "apusr_qsad_swar.hlsl", []),
    )
    common = ["-spirv", "-E", "main", "-T", "cs_6_0",
              f"-fspv-target-env={args.target_env}"]

    cases: list[CaseResult] = []
    failed = False
    for name, shader, flags in specs:
        spv_path = out_dir / f"{name}.spv"
        compile_result = run([dxc, *common, *flags, str(shader), "-Fo", str(spv_path)])
        case = CaseResult(name=name, shader=str(shader), flags=flags,
                          compile=compile_result, spv=str(spv_path))
        cases.append(case)
        print(f"[{name}] DXC -> {compile_result.returncode}")
        if compile_result.returncode != 0:
            print(compile_result.stderr, end="")
            failed = True
            continue

        if spirv_dis:
            asm_path = out_dir / f"{name}.spvasm"
            dis = run([spirv_dis, str(spv_path), "-o", str(asm_path)])
            if dis.returncode != 0:
                print(dis.stderr, end="")
                failed = True
            elif asm_path.exists():
                case.spirv_disassembly = str(asm_path)
                case.spirv_counts = spirv_counts(
                    asm_path.read_text(encoding="utf-8", errors="replace")
                )
                print(f"[{name}] SPIR-V -> {case.spirv_counts}")

        if driver_probe:
            isa_path = out_dir / f"{name}.driver.isa"
            result = run([driver_probe, str(spv_path), str(isa_path)])
            case.driver_probe = result
            if result.returncode != 0:
                print(result.stderr, end="")
                failed = True
            elif isa_path.exists():
                case.driver_isa = str(isa_path)
                case.driver_sad_counts = sad_counts(isa_path)
            print(f"[{name}] installed driver -> {result.returncode}: {case.driver_sad_counts}")

        if rga and args.rga_live:
            isa_path = out_dir / f"{name}.rga-live.isa"
            argv = [rga, "-s", "vulkan", "--comp", str(spv_path), "--isa", str(isa_path)]
            if args.rga_pso:
                argv += ["--pso", str(Path(args.rga_pso).resolve())]
            result = run(argv)
            case.rga_live = result
            actual = locate_output(isa_path)
            if result.returncode != 0:
                print(result.stderr, end="")
                failed = True
            elif actual is not None:
                case.rga_live_isa = str(actual)
                case.rga_live_sad_counts = sad_counts(actual)
            print(f"[{name}] RGA live -> {result.returncode}: {case.rga_live_sad_counts}")

        if rga and args.rga_target:
            isa_path = out_dir / f"{name}.rga-offline.{args.rga_target}.isa"
            result = run([
                rga, "-s", "vk-spv-offline", "-c", args.rga_target,
                "--isa", str(isa_path), str(spv_path),
            ])
            case.rga_offline = result
            actual = locate_output(isa_path)
            if result.returncode != 0:
                print(result.stderr, end="")
                failed = True
            elif actual is not None:
                case.rga_offline_isa = str(actual)
                case.rga_offline_sad_counts = sad_counts(actual)
            print(f"[{name}] RGA offline -> {result.returncode}: {case.rga_offline_sad_counts}")

    report = {
        "schema": 1,
        "dxc": dxc,
        "spirv_dis": spirv_dis,
        "driver_probe": driver_probe,
        "rga": rga,
        "rga_target": args.rga_target,
        "cases": [case_dict(case) for case in cases],
    }
    (out_dir / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    write_markdown(out_dir / "REPORT.md", cases)
    print(f"report: {out_dir / 'REPORT.md'}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
