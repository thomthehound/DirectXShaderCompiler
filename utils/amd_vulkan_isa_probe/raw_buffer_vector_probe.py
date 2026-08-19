#!/usr/bin/env python3
"""Qualify legacy and SPV_KHR_untyped_pointers ByteAddressBuffer codegen."""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
from dataclasses import asdict, dataclass, field
from pathlib import Path


LEGACY_ATOMIC64_DIAGNOSTIC = (
    "64-bit RWByteAddressBuffer atomics require SPV_KHR_untyped_pointers; "
    "the legacy raw-buffer representation cannot provide a native 64-bit atomic pointer"
)


@dataclass
class Case:
    name: str
    kind: str
    shader: str
    extension: str | None = None
    compile_rc: int | None = None
    dis_rc: int | None = None
    val_rc: int | None = None
    classification: str = "not-run"
    counts: dict[str, int] = field(default_factory=dict)
    facts: dict[str, int | bool] = field(default_factory=dict)
    notes: list[str] = field(default_factory=list)


OPS = (
    "OpTypeUntypedPointerKHR",
    "OpUntypedVariableKHR",
    "OpUntypedAccessChainKHR",
    "OpUntypedArrayLengthKHR",
    "OpArrayLength",
    "OpAtomicIAdd",
    "OpAtomicSMin",
    "OpAtomicUMin",
    "OpAtomicCompareExchange",
    "OpLoad",
    "OpStore",
    "OpCompositeConstruct",
    "OpCompositeExtract",
)


def tool(value: str | None, fallback: str) -> str | None:
    if value:
        path = Path(value)
        return str(path.resolve()) if path.exists() else shutil.which(value)
    return shutil.which(fallback)


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


def append_process_notes(
    case: Case, label: str, cp: subprocess.CompletedProcess[str]
) -> None:
    if cp.stdout.strip():
        case.notes.append(f"{label} stdout:\n{cp.stdout.strip()}")
    if cp.stderr.strip():
        case.notes.append(f"{label} stderr:\n{cp.stderr.strip()}")


def print_case(case: Case) -> None:
    print(f"[{case.name}] {case.classification}")
    for note in case.notes:
        print(note)


def make_compile_command(
    dxc: str,
    case: Case,
    target_env: str,
    shader_profile: str,
    shader_path: Path,
    spv_path: Path,
) -> list[str]:
    cmd = [
        dxc,
        "-spirv",
        "-E",
        "main",
        "-T",
        shader_profile,
        f"-fspv-target-env={target_env}",
    ]
    if case.extension:
        cmd.append(f"-fspv-extension={case.extension}")
    cmd.extend([str(shader_path), "-Fo", str(spv_path)])
    return cmd


def analyze(text: str) -> tuple[dict[str, int], dict[str, int | bool]]:
    counts = {op: len(re.findall(rf"\b{op}\b", text)) for op in OPS}
    chain_ids = set(
        re.findall(r"^\s*(%\S+)\s*=\s*OpUntypedAccessChainKHR\b", text, re.M)
    )
    untyped_type_ids = set(
        re.findall(r"^\s*(%\S+)\s*=\s*OpTypeUntypedPointerKHR\b", text, re.M)
    )
    atomic_ptrs = re.findall(
        r"^\s*%\S+\s*=\s*OpAtomic(?:IAdd|SMin|UMin|CompareExchange)\s+%\S+\s+(%\S+)",
        text,
        re.M,
    )
    int64_types = set(
        re.findall(r"^\s*(%\S+)\s*=\s*OpTypeInt\s+64\s+[01]\s*$", text, re.M)
    )
    atomic64_matches = re.findall(
        r"^\s*%\S+\s*=\s*OpAtomic(?:IAdd|SMin|UMin|CompareExchange)\s+(%\S+)\s+(%\S+)",
        text,
        re.M,
    )
    loaded_types = re.findall(r"^\s*%\S+\s*=\s*OpLoad\s+(%\S+)", text, re.M)
    facts: dict[str, int | bool] = {
        "extension": 'OpExtension "SPV_KHR_untyped_pointers"' in text,
        "capability": bool(re.search(r"\bOpCapability\s+UntypedPointersKHR\b", text)),
        "vars_with_data_type": len(
            re.findall(r"OpUntypedVariableKHR\s+%\S+\s+\w+\s+%\S+", text)
        ),
        "aligned4_loads": len(re.findall(r"\bOpLoad\b[^\n]*\bAligned\s+4\b", text)),
        "aligned4_stores": len(
            re.findall(r"\bOpStore\b[^\n]*\bAligned\s+4\b", text)
        ),
        "atomic_uses_untyped_chain": bool(atomic_ptrs)
        and all(ptr in chain_ids for ptr in atomic_ptrs),
        "int64_capability": bool(re.search(r"\bOpCapability\s+Int64\b", text)),
        "int64_atomics_capability": bool(
            re.search(r"\bOpCapability\s+Int64Atomics\b", text)
        ),
        "atomic64_count": sum(ty in int64_types for ty, _ in atomic64_matches),
        "atomic64_uses_untyped_chain": bool(atomic64_matches)
        and all(ty in int64_types and ptr in chain_ids for ty, ptr in atomic64_matches),
        "untyped_pointer_loads": sum(t in untyped_type_ids for t in loaded_types),
    }
    return counts, facts


def classify(case: Case) -> None:
    c, f = case.counts, case.facts
    untyped = bool(f["extension"] or f["capability"] or c["OpUntypedVariableKHR"])

    if case.kind == "vector":
        if case.name.startswith("untyped-"):
            ok = (
                f["extension"]
                and f["capability"]
                and c["OpUntypedVariableKHR"] >= 2
                and int(f["vars_with_data_type"]) >= 2
                and c["OpUntypedAccessChainKHR"] >= 4
                and c["OpLoad"] == 2
                and c["OpStore"] == 2
                and int(f["aligned4_loads"]) == 2
                and int(f["aligned4_stores"]) == 2
                and c["OpCompositeConstruct"] == 0
                and c["OpCompositeExtract"] == 0
            )
            case.classification = (
                "untyped-vectorized" if ok else "untyped-vector-partial"
            )
        else:
            ok = not untyped and c["OpLoad"] >= 8 and c["OpStore"] >= 8
            case.classification = "typed-scalarized" if ok else "legacy-vector-changed"
        return

    if case.kind == "atomic64":
        ok = (
            case.name.startswith("untyped-")
            and f["extension"]
            and f["capability"]
            and f["int64_capability"]
            and f["int64_atomics_capability"]
            and c["OpUntypedVariableKHR"] >= 1
            and int(f["vars_with_data_type"]) >= 1
            and c["OpUntypedAccessChainKHR"] >= 4
            and c["OpAtomicIAdd"] == 1
            and c["OpAtomicSMin"] == 1
            and c["OpAtomicUMin"] == 1
            and c["OpAtomicCompareExchange"] == 1
            and int(f["atomic64_count"]) == 4
            and f["atomic64_uses_untyped_chain"]
        )
        case.classification = (
            "untyped-atomic64-native" if ok else "untyped-atomic64-partial"
        )
        return

    if case.kind == "surface":
        if case.name.startswith("untyped-"):
            ok = (
                f["extension"]
                and f["capability"]
                and c["OpUntypedVariableKHR"] >= 2
                and int(f["vars_with_data_type"]) >= 2
                and c["OpUntypedArrayLengthKHR"] >= 1
                and c["OpArrayLength"] == 0
                and c["OpAtomicIAdd"] >= 1
                and f["atomic_uses_untyped_chain"]
            )
            case.classification = (
                "untyped-surface-complete" if ok else "untyped-surface-partial"
            )
        else:
            ok = (
                not untyped
                and c["OpUntypedArrayLengthKHR"] == 0
                and c["OpArrayLength"] >= 1
                and c["OpAtomicIAdd"] >= 1
            )
            case.classification = (
                "typed-surface-compatible" if ok else "legacy-surface-changed"
            )
        return

    if case.name.startswith("untyped-"):
        ok = (
            f["extension"]
            and f["capability"]
            and c["OpUntypedVariableKHR"] >= 2
            and int(f["vars_with_data_type"]) >= 2
            and c["OpUntypedAccessChainKHR"] >= 2
            and int(f["untyped_pointer_loads"]) >= 1
            and c["OpCompositeConstruct"] == 0
            and c["OpCompositeExtract"] == 0
        )
        case.classification = (
            "untyped-alias-complete" if ok else "untyped-alias-partial"
        )
    else:
        ok = (
            not untyped
            and c["OpCompositeConstruct"] >= 1
            and c["OpCompositeExtract"] >= 4
        )
        case.classification = "typed-alias-compatible" if ok else "legacy-alias-changed"


def self_test() -> int:
    fixtures = {
        ("legacy-vector", "vector"): """
%a=OpLoad %u %p
%b=OpLoad %u %p
%c=OpLoad %u %p
%d=OpLoad %u %p
%e=OpLoad %u %p
%f=OpLoad %u %p
%g=OpLoad %u %p
%h=OpLoad %u %p
OpStore %p %a
OpStore %p %b
OpStore %p %c
OpStore %p %d
OpStore %p %e
OpStore %p %f
OpStore %p %g
OpStore %p %h
""",
        ("untyped-vector", "vector"): """
OpCapability UntypedPointersKHR
OpExtension "SPV_KHR_untyped_pointers"
%i=OpUntypedVariableKHR %up Uniform %Raw
%o=OpUntypedVariableKHR %up Uniform %Raw
%p0=OpUntypedAccessChainKHR %up4 %Raw %i %z %n
%a=OpLoad %v4 %p0 Aligned 4
%p1=OpUntypedAccessChainKHR %up4 %Raw %i %z %n
%b=OpLoad %v4 %p1 Aligned 4
%p2=OpUntypedAccessChainKHR %up4 %Raw %o %z %n
OpStore %p2 %a Aligned 4
%p3=OpUntypedAccessChainKHR %up4 %Raw %o %z %n
OpStore %p3 %b Aligned 4
""",
        ("legacy-surface", "surface"): """
%n=OpArrayLength %u %i 0
%old=OpAtomicIAdd %u %p %scope %sem %n
""",
        ("untyped-surface", "surface"): """
OpCapability UntypedPointersKHR
OpExtension "SPV_KHR_untyped_pointers"
%i=OpUntypedVariableKHR %up Uniform %Raw
%o=OpUntypedVariableKHR %up Uniform %Raw
%n=OpUntypedArrayLengthKHR %u %Raw %i 0
%p=OpUntypedAccessChainKHR %up32 %Raw %o %z %idx
%old=OpAtomicIAdd %u %p %scope %sem %n
""",
        ("legacy-alias", "alias"): """
%a=OpLoad %u %p
%b=OpLoad %u %p
%c=OpLoad %u %p
%d=OpLoad %u %p
%v=OpCompositeConstruct %v4 %a %b %c %d
%x0=OpCompositeExtract %u %v 0
%x1=OpCompositeExtract %u %v 1
%x2=OpCompositeExtract %u %v 2
%x3=OpCompositeExtract %u %v 3
""",
        ("untyped-alias", "alias"): """
OpCapability UntypedPointersKHR
OpExtension "SPV_KHR_untyped_pointers"
%up=OpTypeUntypedPointerKHR Uniform
%i=OpUntypedVariableKHR %up Uniform %Raw
%o=OpUntypedVariableKHR %up Uniform %Raw
%r=OpLoad %up %alias
%p0=OpUntypedAccessChainKHR %up4 %Raw %r %z %n
%v=OpLoad %v4 %p0 Aligned 4
%w=OpLoad %up %alias2
%p1=OpUntypedAccessChainKHR %up4 %Raw %w %z %n
OpStore %p1 %v Aligned 4
""",
        ("untyped-atomic64", "atomic64"): """
OpCapability Int64
OpCapability Int64Atomics
OpCapability UntypedPointersKHR
OpExtension "SPV_KHR_untyped_pointers"
%u64=OpTypeInt 64 0
%s64=OpTypeInt 64 1
%up=OpTypeUntypedPointerKHR Uniform
%o=OpUntypedVariableKHR %up Uniform %Raw
%p0=OpUntypedAccessChainKHR %up %Raw %o %z %n0
%a=OpAtomicIAdd %u64 %p0 %scope %sem %one
%p1=OpUntypedAccessChainKHR %up %Raw %o %z %n1
%b=OpAtomicSMin %s64 %p1 %scope %sem %neg
%p2=OpUntypedAccessChainKHR %up %Raw %o %z %n2
%c=OpAtomicUMin %u64 %p2 %scope %sem %nine
%p3=OpUntypedAccessChainKHR %up %Raw %o %z %n3
%d=OpAtomicCompareExchange %u64 %p3 %scope %eq %uneq %four %three
""",
    }
    expected = {
        "legacy-vector": "typed-scalarized",
        "untyped-vector": "untyped-vectorized",
        "legacy-surface": "typed-surface-compatible",
        "untyped-surface": "untyped-surface-complete",
        "legacy-alias": "typed-alias-compatible",
        "untyped-alias": "untyped-alias-complete",
        "untyped-atomic64": "untyped-atomic64-native",
    }
    for (name, kind), text in fixtures.items():
        case = Case(name, kind, "fixture")
        case.counts, case.facts = analyze(text)
        classify(case)
        if case.classification != expected[name]:
            print(f"FAIL {name}: {case.classification} != {expected[name]}")
            return 1

    legacy = Case("legacy-vector", "vector", "fixture")
    untyped = Case(
        "untyped-vector", "vector", "fixture", "SPV_KHR_untyped_pointers"
    )
    legacy_cmd = make_compile_command(
        "dxc", legacy, "vulkan1.2", "cs_6_6", Path("a.hlsl"), Path("a.spv")
    )
    untyped_cmd = make_compile_command(
        "dxc", untyped, "vulkan1.2", "cs_6_6", Path("a.hlsl"), Path("a.spv")
    )
    if any(arg.startswith("-fspv-extension=") for arg in legacy_cmd):
        print("FAIL legacy command unexpectedly enables an extension")
        return 1
    if "-fspv-extension=SPV_KHR_untyped_pointers" not in untyped_cmd:
        print("FAIL untyped command does not enable SPV_KHR_untyped_pointers")
        return 1

    print("raw-buffer SPIR-V contract parser/command self-test: PASS")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dxc")
    ap.add_argument("--spirv-dis", dest="spirv_dis")
    ap.add_argument("--spirv-val", dest="spirv_val")
    ap.add_argument("--target-env", default="vulkan1.2")
    ap.add_argument("--shader-profile", default="cs_6_6")
    ap.add_argument("--out-dir", default="out/raw-buffer-vector-probe")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--require-untyped-vectorization", action="store_true")
    ap.add_argument("--require-untyped-contract", action="store_true")
    ap.add_argument("--require-validation", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()

    dxc = tool(args.dxc, "dxc")
    dis = tool(args.spirv_dis, "spirv-dis")
    val = tool(args.spirv_val, "spirv-val")
    if not dxc or not dis:
        ap.error("DXC and spirv-dis are required; pass --dxc/--spirv-dis")
    if args.require_validation and not val:
        ap.error("spirv-val is required by --require-validation")

    root = Path(__file__).resolve().parent
    out = Path(args.out_dir).resolve()
    out.mkdir(parents=True, exist_ok=True)
    untyped_ext = "SPV_KHR_untyped_pointers"
    specs = (
        Case("legacy-vector", "vector", "shaders/raw_buffer_vector.hlsl"),
        Case(
            "untyped-vector", "vector", "shaders/raw_buffer_vector.hlsl", untyped_ext
        ),
        Case("legacy-surface", "surface", "shaders/raw_buffer_surface.hlsl"),
        Case(
            "untyped-surface", "surface", "shaders/raw_buffer_surface.hlsl", untyped_ext
        ),
        Case("legacy-alias", "alias", "shaders/raw_buffer_alias.hlsl"),
        Case(
            "untyped-alias", "alias", "shaders/raw_buffer_alias.hlsl", untyped_ext
        ),
        Case("legacy-atomic64", "atomic64", "shaders/raw_buffer_atomic64.hlsl"),
        Case(
            "untyped-atomic64",
            "atomic64",
            "shaders/raw_buffer_atomic64.hlsl",
            untyped_ext,
        ),
    )

    failed = False
    for case in specs:
        spv = out / f"{case.name}.spv"
        asm = out / f"{case.name}.spvasm"
        for path in (spv, asm):
            if path.exists():
                path.unlink()

        cmd = make_compile_command(
            dxc,
            case,
            args.target_env,
            args.shader_profile,
            root / case.shader,
            spv,
        )
        cp = run(cmd)
        case.compile_rc = cp.returncode
        if cp.returncode or not spv.exists():
            append_process_notes(case, "dxc", cp)
            if (
                case.name == "legacy-atomic64"
                and cp.returncode
                and LEGACY_ATOMIC64_DIAGNOSTIC in (cp.stdout + cp.stderr)
            ):
                case.classification = "legacy-atomic64-rejected"
            elif case.name == "legacy-atomic64" and cp.returncode:
                case.classification = "legacy-atomic64-wrong-rejection"
                failed = True
            else:
                case.classification = "compile-failed"
                failed = True
            print_case(case)
            continue

        if val:
            vp = run([val, "--target-env", args.target_env, str(spv)])
            case.val_rc = vp.returncode
            if vp.returncode:
                append_process_notes(case, "spirv-val", vp)
                failed = True

        dp = run([dis, str(spv), "-o", str(asm)])
        case.dis_rc = dp.returncode
        if dp.returncode or not asm.exists():
            append_process_notes(case, "spirv-dis", dp)
            case.classification = "disassembly-failed"
            failed = True
            print_case(case)
            continue

        case.counts, case.facts = analyze(
            asm.read_text(encoding="utf-8", errors="replace")
        )
        classify(case)
        print_case(case)

    by_name = {c.name: c for c in specs}
    legacy_ready = all(
        by_name[name].classification == expected
        for name, expected in (
            ("legacy-vector", "typed-scalarized"),
            ("legacy-surface", "typed-surface-compatible"),
            ("legacy-alias", "typed-alias-compatible"),
        )
    )
    vector_ready = by_name["untyped-vector"].classification == "untyped-vectorized"
    surface_ready = (
        by_name["untyped-surface"].classification == "untyped-surface-complete"
    )
    alias_ready = by_name["untyped-alias"].classification == "untyped-alias-complete"
    atomic64_ready = (
        by_name["legacy-atomic64"].classification == "legacy-atomic64-rejected"
        and by_name["untyped-atomic64"].classification == "untyped-atomic64-native"
    )
    contract_ready = (
        legacy_ready
        and vector_ready
        and surface_ready
        and alias_ready
        and atomic64_ready
    )
    validation_ready = bool(val) and all(
        c.val_rc == 0 for c in specs if c.name != "legacy-atomic64"
    )

    if args.require_untyped_vectorization and not vector_ready:
        failed = True
    if args.require_untyped_contract and not contract_ready:
        failed = True
    if args.require_validation and not validation_ready:
        failed = True

    report = {
        "schema": 5,
        "target_env": args.target_env,
        "shader_profile": args.shader_profile,
        "legacy_compatibility_ready": legacy_ready,
        "untyped_vectorization_ready": vector_ready,
        "untyped_surface_ready": surface_ready,
        "untyped_alias_ready": alias_ready,
        "native_raw_atomic64_ready": atomic64_ready,
        "spirv_validation_ready": validation_ready,
        "untyped_raw_buffer_contract_ready": contract_ready,
        "cases": [asdict(c) for c in specs],
    }
    (out / "report.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, indent=2))
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
