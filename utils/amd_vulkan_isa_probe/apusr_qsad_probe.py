#!/usr/bin/env python3
"""Compare APUSR QSad/MSAD lowering shapes through SPIR-V and Radeon ISA.

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
EXACT_MQSAD = "v_mqsad_u32_u8"


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
    spv: str | None = None
    spirv_disassembly: str | None = None
    spirv_status: str = "not-probed"
    spirv_counts: dict[str, int] = field(default_factory=dict)
    shape_notes: list[str] = field(default_factory=list)
    driver_probe: CommandResult | None = None
    driver_status: str = "not-probed"
    driver_isa: str | None = None
    driver_sad_counts: dict[str, int] = field(default_factory=dict)
    rga_live: CommandResult | None = None
    rga_live_status: str = "not-probed"
    rga_live_isa: str | None = None
    rga_live_sad_counts: dict[str, int] = field(default_factory=dict)
    rga_offline: CommandResult | None = None
    rga_offline_status: str = "not-probed"
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


def spirv_counts(text: str) -> dict[str, int]:
    ops = (
        "OpUDot",
        "OpSDot",
        "OpSUDot",
        "OpIAdd",
        "OpISub",
        "OpSAbs",
        "OpBitwiseAnd",
        "OpBitwiseOr",
        "OpBitwiseXor",
        "OpShiftRightLogical",
        "OpShiftLeftLogical",
        "OpSelect",
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


def clear_output_candidates(requested: Path) -> None:
    """Remove outputs from an earlier probe so they cannot masquerade as new."""
    candidates = {requested}
    if requested.parent.exists():
        candidates.update(
            requested.parent.glob(f"*{requested.stem}*{requested.suffix}")
        )
    for path in candidates:
        if path.is_file():
            path.unlink()


def locate_output(requested: Path) -> Path | None:
    if requested.exists():
        return requested
    matches = [
        path
        for path in requested.parent.glob(
            f"*{requested.stem}*{requested.suffix}"
        )
        if path.is_file()
    ]
    return max(matches, key=lambda p: p.stat().st_mtime_ns) if matches else None


def evidence_status(
    command: CommandResult | None,
    isa_path: Path | None,
    counts: dict[str, int],
) -> str:
    if command is None:
        return "not-probed"
    if command.returncode != 0:
        return "probe-failed"
    if isa_path is None or not isa_path.exists():
        return "missing-output"
    if counts.get(EXACT_MQSAD, 0):
        return "native-mqsad"
    if counts:
        return "other-sad-family"
    return "no-sad-family"


def fmt_evidence(status: str, counts: dict[str, int]) -> str:
    if status == "not-probed":
        return "not probed"
    if status == "probe-failed":
        return "probe failed"
    if status == "missing-output":
        return "missing ISA output"
    if counts:
        return ", ".join(f"{name} x{count}" for name, count in counts.items())
    return "no SAD-family ISA"


def driver_verdict(cases: list[CaseResult]) -> dict[str, object]:
    exact = [
        case.name
        for case in cases
        if case.driver_sad_counts.get(EXACT_MQSAD, 0) > 0
    ]
    other = [
        case.name
        for case in cases
        if case.driver_status == "other-sad-family"
    ]
    none = [
        case.name
        for case in cases
        if case.driver_status == "no-sad-family"
    ]
    failed = [
        case.name
        for case in cases
        if case.driver_status in {"probe-failed", "missing-output"}
    ]
    not_probed = [
        case.name for case in cases if case.driver_status == "not-probed"
    ]

    if exact:
        verdict = "mqsad-recovered"
    elif failed:
        verdict = "incomplete-driver-probe"
    elif not_probed:
        verdict = "driver-not-probed"
    elif other:
        verdict = "sad-family-but-no-mqsad"
    else:
        verdict = "no-sad-family-from-any-shape"

    return {
        "verdict": verdict,
        "exact_mqsad_cases": exact,
        "other_sad_family_cases": other,
        "no_sad_family_cases": none,
        "failed_cases": failed,
        "not_probed_cases": not_probed,
    }


def command_dict(value: CommandResult | None) -> dict[str, object] | None:
    return asdict(value) if value is not None else None


def case_dict(value: CaseResult) -> dict[str, object]:
    result = asdict(value)
    for field_name in ("compile", "driver_probe", "rga_live", "rga_offline"):
        result[field_name] = command_dict(getattr(value, field_name))
    return result


def add_shape_notes(case: CaseResult) -> None:
    """Record strong shape expectations without turning optimizer drift into failure."""
    if case.spirv_status != "ok":
        return

    observed_udot = case.spirv_counts.get("OpUDot", 0)
    if case.name in {"apusr.qsad.msad4-udot", "apusr.qsad.swar-udot"}:
        # 8 rows x 2 QSad/MSAD operations x 4 packed byte lanes.
        expected_udot = 64
        if observed_udot == expected_udot:
            case.shape_notes.append("expected OpUDot multiplicity: 64")
        else:
            case.shape_notes.append(
                f"OpUDot multiplicity changed: expected 64, observed {observed_udot}"
            )


def write_markdown(
    path: Path,
    cases: list[CaseResult],
    profile: str,
    target_env: str,
) -> None:
    verdict = driver_verdict(cases)
    lines = [
        "# APUSR QSad native-lowering comparison",
        "",
        f"Shader profile: `{profile}`. SPIR-V target: `{target_env}`.",
        "",
        "The three cases model the same 8-row x 2-QSad optical-flow search shape:",
        "ordinary DXC `msad4` expansion, the AMD-oriented packed-UDOT `msad4` expansion,",
        "and APUSR's current SWAR+packed-UDOT Vulkan path.",
        "",
        f"Installed-driver verdict: **{verdict['verdict']}**.",
        "",
        "| Case | SPIR-V shape | Installed driver | RGA live | RGA offline |",
        "|---|---|---|---|---|",
    ]
    for case in cases:
        if case.spirv_status == "ok":
            spv = (
                ", ".join(
                    f"{key}={value}"
                    for key, value in case.spirv_counts.items()
                    if value
                )
                or "no counted ops"
            )
        else:
            spv = case.spirv_status
        lines.append(
            f"| `{case.name}` | {spv} | "
            f"{fmt_evidence(case.driver_status, case.driver_sad_counts)} | "
            f"{fmt_evidence(case.rga_live_status, case.rga_live_sad_counts)} | "
            f"{fmt_evidence(case.rga_offline_status, case.rga_offline_sad_counts)} |"
        )

    lines += [
        "",
        "## Installed-driver classification",
        "",
        f"- Exact `v_mqsad_u32_u8`: {', '.join(verdict['exact_mqsad_cases']) or 'none'}",
        f"- Other SAD-family ISA: {', '.join(verdict['other_sad_family_cases']) or 'none'}",
        f"- Probed with no SAD-family ISA: {', '.join(verdict['no_sad_family_cases']) or 'none'}",
        f"- Probe failures/missing output: {', '.join(verdict['failed_cases']) or 'none'}",
        f"- Not probed: {', '.join(verdict['not_probed_cases']) or 'none'}",
        "",
        "## SPIR-V shape notes",
        "",
    ]
    for case in cases:
        notes = "; ".join(case.shape_notes) if case.shape_notes else "none"
        lines.append(f"- `{case.name}`: {notes}")

    lines += [
        "",
        "## Interpretation",
        "",
        "- Installed-driver ISA decides whether the current Radeon stack recovers `v_mqsad_u32_u8` from any legal SPIR-V spelling.",
        "- Comparing the two `msad4` cases isolates whether packed UDOT helps or destroys native MQSAD recognition.",
        "- The SWAR case tells us whether APUSR's current production workaround is already recoverable below DXC.",
        "- RGA offline is target evidence only; it is not substituted for installed-driver proof.",
        "- No SAD-family mnemonic is a valid negative result when ISA extraction succeeds.",
        "- Compare instruction counts as well as mnemonic presence: partial recovery can still leave substantial integer work.",
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
                lines.append(
                    f"- `{case.name}` / {label}: "
                    f"`{quote_cmd(result.argv)}` -> `{result.returncode}`"
                )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--dxc", help="DXC executable containing the AMD SPIR-V work"
    )
    parser.add_argument("--spirv-dis", dest="spirv_dis", help="spirv-dis executable")
    parser.add_argument("--driver-probe", help="amd_vulkan_isa_probe executable")
    parser.add_argument("--rga", help="RGA CLI executable")
    parser.add_argument("--rga-target", help="Offline RGA target, e.g. gfx1151")
    parser.add_argument("--rga-live", action="store_true")
    parser.add_argument("--rga-pso", help="Optional .cpso for RGA live mode")
    parser.add_argument("--target-env", default="vulkan1.2")
    parser.add_argument(
        "--shader-profile",
        default="cs_6_6",
        help="DXC shader profile; defaults to APUSR Vulkan framegen's cs_6_6",
    )
    parser.add_argument("--out-dir", default="out/apusr-qsad-probe")
    args = parser.parse_args()

    dxc = resolve_tool(args.dxc, "dxc")
    if not dxc:
        parser.error("DXC not found; pass --dxc")
    spirv_dis = resolve_tool(args.spirv_dis, "spirv-dis")
    driver_probe = resolve_tool(args.driver_probe, "amd_vulkan_isa_probe")
    rga = resolve_tool(args.rga, "rga") or (
        None if args.rga else resolve_tool(None, "rga.exe")
    )

    here = Path(__file__).resolve().parent
    shaders = here / "shaders"
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    specs = (
        ("apusr.qsad.msad4-scalar", shaders / "apusr_qsad_msad4.hlsl", []),
        (
            "apusr.qsad.msad4-udot",
            shaders / "apusr_qsad_msad4.hlsl",
            ["-fspv-enable-amd-intrinsics"],
        ),
        ("apusr.qsad.swar-udot", shaders / "apusr_qsad_swar.hlsl", []),
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
    for name, shader, flags in specs:
        spv_path = out_dir / f"{name}.spv"
        clear_output_candidates(spv_path)
        compile_result = run(
            [dxc, *common, *flags, str(shader), "-Fo", str(spv_path)]
        )
        case = CaseResult(
            name=name,
            shader=str(shader),
            flags=flags,
            compile=compile_result,
            spv=str(spv_path),
        )
        cases.append(case)

        if compile_result.returncode != 0:
            case.compile_status = "failed"
            print(f"[{name}] DXC -> {compile_result.returncode}")
            print(compile_result.stderr, end="")
            failed = True
            continue
        if not spv_path.exists():
            case.compile_status = "missing-output"
            print(f"[{name}] DXC returned 0 but produced no SPIR-V: {spv_path}")
            failed = True
            continue
        case.compile_status = "ok"
        print(f"[{name}] DXC -> 0")

        if spirv_dis:
            asm_path = out_dir / f"{name}.spvasm"
            clear_output_candidates(asm_path)
            dis = run([spirv_dis, str(spv_path), "-o", str(asm_path)])
            if dis.returncode != 0:
                case.spirv_status = "disassembly-failed"
                print(dis.stderr, end="")
                failed = True
            elif not asm_path.exists():
                case.spirv_status = "missing-output"
                print(f"[{name}] spirv-dis returned 0 but produced no output")
                failed = True
            else:
                case.spirv_status = "ok"
                case.spirv_disassembly = str(asm_path)
                case.spirv_counts = spirv_counts(
                    asm_path.read_text(encoding="utf-8", errors="replace")
                )
                add_shape_notes(case)
                print(f"[{name}] SPIR-V -> {case.spirv_counts}")

        if driver_probe:
            isa_path = out_dir / f"{name}.driver.isa"
            clear_output_candidates(isa_path)
            result = run([driver_probe, str(spv_path), str(isa_path)])
            case.driver_probe = result
            actual = locate_output(isa_path) if result.returncode == 0 else None
            if actual is not None:
                case.driver_isa = str(actual)
                case.driver_sad_counts = sad_counts(actual)
            case.driver_status = evidence_status(
                result, actual, case.driver_sad_counts
            )
            if case.driver_status in {"probe-failed", "missing-output"}:
                print(result.stderr, end="")
                failed = True
            print(
                f"[{name}] installed driver -> {case.driver_status}: "
                f"{case.driver_sad_counts}"
            )

        if rga and args.rga_live:
            isa_path = out_dir / f"{name}.rga-live.isa"
            clear_output_candidates(isa_path)
            argv = [
                rga,
                "-s",
                "vulkan",
                "--comp",
                str(spv_path),
                "--isa",
                str(isa_path),
            ]
            if args.rga_pso:
                argv += ["--pso", str(Path(args.rga_pso).resolve())]
            result = run(argv)
            case.rga_live = result
            actual = locate_output(isa_path) if result.returncode == 0 else None
            if actual is not None:
                case.rga_live_isa = str(actual)
                case.rga_live_sad_counts = sad_counts(actual)
            case.rga_live_status = evidence_status(
                result, actual, case.rga_live_sad_counts
            )
            if case.rga_live_status in {"probe-failed", "missing-output"}:
                print(result.stderr, end="")
                failed = True
            print(
                f"[{name}] RGA live -> {case.rga_live_status}: "
                f"{case.rga_live_sad_counts}"
            )

        if rga and args.rga_target:
            isa_path = out_dir / f"{name}.rga-offline.{args.rga_target}.isa"
            clear_output_candidates(isa_path)
            result = run(
                [
                    rga,
                    "-s",
                    "vk-spv-offline",
                    "-c",
                    args.rga_target,
                    "--isa",
                    str(isa_path),
                    str(spv_path),
                ]
            )
            case.rga_offline = result
            actual = locate_output(isa_path) if result.returncode == 0 else None
            if actual is not None:
                case.rga_offline_isa = str(actual)
                case.rga_offline_sad_counts = sad_counts(actual)
            case.rga_offline_status = evidence_status(
                result, actual, case.rga_offline_sad_counts
            )
            if case.rga_offline_status in {"probe-failed", "missing-output"}:
                print(result.stderr, end="")
                failed = True
            print(
                f"[{name}] RGA offline -> {case.rga_offline_status}: "
                f"{case.rga_offline_sad_counts}"
            )

    verdict = driver_verdict(cases)
    report = {
        "schema": 2,
        "dxc": dxc,
        "spirv_dis": spirv_dis,
        "driver_probe": driver_probe,
        "rga": rga,
        "rga_target": args.rga_target,
        "shader_profile": args.shader_profile,
        "target_env": args.target_env,
        "installed_driver_verdict": verdict,
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
    print(f"installed-driver verdict: {verdict['verdict']}")
    print(f"report: {out_dir / 'REPORT.md'}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
