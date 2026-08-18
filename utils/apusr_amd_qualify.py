#!/usr/bin/env python3
"""Qualify the AMD-oriented DXC branch against compiler contracts and APUSR shaders.

The harness is deliberately layered:
  inventory  - verify the acceleration matrix has no silently unattempted rows
               and report APUSR consumer evidence.
  compiler   - inventory + lightweight DXC SPIR-V/math contract suites.
  smoke      - compiler + representative APUSR FrameGen and FSR4 DXIL/SPIR-V
               compilation using the exact DXC binary under test.
  qualify    - compiler + the complete FrameGen permutation packs and one full
               FSR4 preset/capacity slice on both DXIL and SPIR-V.

It never overwrites APUSR shader packs or its bundled compiler. Temporary files
are used for every APUSR compilation performed here.
"""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import importlib.util
import json
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from types import ModuleType
from typing import Iterable


ROOT = Path(__file__).resolve().parents[1]
MATRIX_PATH = ROOT / "utils/apusr_amd_acceleration_matrix.json"
VALID_ATTEMPTS = {"implemented", "probe-only", "blocked", "deferred-hardware"}

SOURCE_PATTERNS = {
    "packed_dot_i8_calls": re.compile(r"\bdot4add_i8packed\s*\("),
    "wave_read_lane_at_calls": re.compile(r"\bWaveReadLaneAt\s*\("),
    "wave_lane_id_calls": re.compile(r"\bWaveGetLaneIndex\s*\("),
    "tanh_calls": re.compile(r"\btanh\s*\("),
    "exp_calls": re.compile(r"\bexp\s*\("),
    "log_calls": re.compile(r"\blog\s*\("),
    "log2_calls": re.compile(r"\blog2\s*\("),
    "pow_calls": re.compile(r"\bpow\s*\("),
    "rcp_calls": re.compile(r"\brcp\s*\("),
    "sqrt_calls": re.compile(r"\bsqrt\s*\("),
    "f16tof32_calls": re.compile(r"\bf16tof32\s*\("),
    "f32tof16_calls": re.compile(r"\bf32tof16\s*\("),
    "groupshared_declarations": re.compile(r"\bgroupshared\b"),
    "barrier_calls": re.compile(
        r"\b(?:GroupMemoryBarrier|DeviceMemoryBarrier|AllMemoryBarrier)[A-Za-z0-9_]*\s*\("
    ),
    "product_interlocked_calls": re.compile(r"\bInterlocked[A-Za-z0-9_]*\s*\("),
}

FAMILY_ALIASES = {
    "all": None,
    "dot": {"packed-dot-4x8", "packed-dot-mixed-2x16-8x4-saturating"},
    "sad": {"sad-msad-qsad-mqsad", "align-permute-byte-routing"},
    "integer": {
        "bitfield-scan-pack", "mul24-mad24-highmul", "fused-integer-valu",
        "align-permute-byte-routing", "minmax-med3-clamp",
    },
    "fp": {
        "fp-transcendental-standard", "fp-decompose-class-rounding",
        "packed-f16-bf16-conversion-dot", "pow-tanh-and-pattern-combines",
        "minmax-med3-clamp",
    },
    "wave": {
        "wave-broadcast-shuffle-ballot", "wave-reduce-scan-cluster",
        "dpp-permlane-crosslane",
    },
    "matrix": {"wmma-swmma-cooperative-matrix"},
    "memory": {"lds-barrier-resource-codegen", "atomics-64-and-contention"},
    "instrumentation": {"clock-instrumentation"},
}


@dataclass(frozen=True)
class CommandResult:
    argv: list[str]
    returncode: int
    stdout: str
    stderr: str


def run(argv: Iterable[os.PathLike[str] | str], *, cwd: Path | None = None) -> CommandResult:
    args = [str(x) for x in argv]
    proc = subprocess.run(
        args,
        cwd=str(cwd) if cwd else None,
        text=True,
        encoding="utf-8",
        errors="replace",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if proc.stdout:
        print(proc.stdout, end="")
    if proc.stderr:
        print(proc.stderr, end="", file=sys.stderr)
    return CommandResult(args, proc.returncode, proc.stdout, proc.stderr)


def require_command(argv: Iterable[os.PathLike[str] | str], *, cwd: Path | None = None) -> None:
    result = run(argv, cwd=cwd)
    if result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(result.argv)}"
        )


def load_matrix(path: Path = MATRIX_PATH) -> dict:
    data = json.loads(path.read_text(encoding="utf-8"))
    families = data.get("families")
    if not isinstance(families, list) or not families:
        raise RuntimeError("acceleration matrix has no families")
    seen: set[str] = set()
    for row in families:
        family_id = row.get("id")
        if not family_id or family_id in seen:
            raise RuntimeError(f"invalid or duplicate matrix family id: {family_id!r}")
        seen.add(family_id)
        attempt = row.get("attempt")
        if attempt not in VALID_ATTEMPTS:
            raise RuntimeError(
                f"{family_id}: attempt must be one of {sorted(VALID_ATTEMPTS)}, got {attempt!r}"
            )
        if attempt == "blocked" and not row.get("note"):
            raise RuntimeError(f"{family_id}: blocked rows require a concrete note")
        for backend in ("spirv", "dxil", "d3d11on12"):
            if not row.get(backend):
                raise RuntimeError(f"{family_id}: missing {backend} disposition")
        if not row.get("tests"):
            raise RuntimeError(f"{family_id}: no qualification test or pending test recorded")
    return data


def selected_family_ids(matrix: dict, family: str) -> set[str] | None:
    if family in FAMILY_ALIASES:
        return FAMILY_ALIASES[family]
    ids = {row["id"] for row in matrix["families"]}
    if family not in ids:
        raise RuntimeError(
            f"unknown family {family!r}; choose an alias {sorted(FAMILY_ALIASES)} or a matrix id"
        )
    return {family}


def print_matrix_status(matrix: dict, family: str) -> None:
    selected = selected_family_ids(matrix, family)
    rows = [row for row in matrix["families"] if selected is None or row["id"] in selected]
    width = max(len(row["id"]) for row in rows)
    for row in rows:
        print(f"[matrix] {row['id']:<{width}}  {row['attempt']}")


def apusr_shader_files(apusr_root: Path) -> list[Path]:
    files = [*apusr_root.rglob("*.hlsl"), *apusr_root.rglob("*.hlsli")]
    out = []
    for path in files:
        parts = set(path.parts)
        posix = path.as_posix()
        if "third_party" in parts or "experiments" in parts or "quarantine" in parts:
            continue
        if "/include/AmdExtD3D/" in posix:
            continue
        out.append(path)
    return out


def scan_apusr(apusr_root: Path) -> dict[str, int]:
    if not apusr_root.is_dir():
        raise RuntimeError(f"APUSR root does not exist: {apusr_root}")
    counts = {name: 0 for name in SOURCE_PATTERNS}
    for path in apusr_shader_files(apusr_root):
        text = path.read_text(encoding="utf-8", errors="ignore")
        for name, pattern in SOURCE_PATTERNS.items():
            counts[name] += len(pattern.findall(text))
    print("[APUSR] shader consumer evidence")
    width = max(len(name) for name in counts)
    for name, count in counts.items():
        print(f"  {name:<{width}} : {count}")
    return counts


def load_module(path: Path, name: str, search_dir: Path | None = None) -> ModuleType:
    if not path.is_file():
        raise RuntimeError(f"required APUSR tool not found: {path}")
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load Python module: {path}")
    module = importlib.util.module_from_spec(spec)
    old_path = list(sys.path)
    if search_dir is not None:
        sys.path.insert(0, str(search_dir))
    try:
        sys.modules[name] = module
        spec.loader.exec_module(module)
    finally:
        sys.path[:] = old_path
    return module


def compiler_contracts(dxc: Path) -> None:
    print("[qualify] DXC AMD SPIR-V contracts")
    require_command([sys.executable, ROOT / "utils/amd_spirv_ci.py", "--dxc", dxc])
    print("[qualify] DXC AMD math contracts")
    require_command([sys.executable, ROOT / "utils/amd_math_ci.py", "--dxc", dxc])


def framegen_smoke(apusr_root: Path, dxc: Path) -> None:
    print("[qualify] APUSR FrameGen DX12 smoke")
    require_command([
        sys.executable,
        apusr_root / "tools/compile_framegen_dx12_shaders.py",
        "--dxc", dxc,
        "--smoke",
    ], cwd=apusr_root)

    print("[qualify] APUSR FrameGen Vulkan representative FI/OF")
    vk_tool = load_module(
        apusr_root / "tools/compile_framegen_vulkan_shaders.py",
        "apusr_framegen_vk_qualify",
        apusr_root / "tools",
    )
    permutations = list(vk_tool.permutations())
    fi = next(item for item in permutations if item[0].effect == vk_tool.EFFECT_FRAME_INTERPOLATION and item[0].pass_id == 6 and not item[3])
    optical_flow = next(item for item in permutations if item[0].effect == vk_tool.EFFECT_OPTICAL_FLOW and item[0].pass_id == 4 and not item[3])
    for label, item in (("FI", fi), ("OF", optical_flow)):
        key, source, defines, fp16 = item
        blob = vk_tool.compile_one(dxc, source, defines, fp16)
        print(
            f"  PASS {label}: pass={key.pass_id} options=0x{key.options:x} "
            f"bytes={len(blob)} sha256={hashlib.sha256(blob).hexdigest()[:16]}"
        )


def fsr4_modules(apusr_root: Path) -> tuple[ModuleType, ModuleType]:
    model_tools = apusr_root / "tools/model"
    old_path = list(sys.path)
    sys.path.insert(0, str(model_tools))
    try:
        dx12 = load_module(
            model_tools / "compile_fsr4_reference_dx12_shaders.py",
            "compile_fsr4_reference_dx12_shaders",
        )
        vk = load_module(
            model_tools / "compile_fsr4_reference_vulkan_shaders.py",
            "compile_fsr4_reference_vulkan_shaders",
        )
    finally:
        sys.path[:] = old_path
    return dx12, vk


def default_fsr4_options(dx12: ModuleType):
    return dx12.PermutationOptions(
        wmma=False,
        depth_inverted=True,
        low_res_motion_vectors=True,
        auto_exposure=True,
        jittered_motion_vectors=False,
        color_space="linear",
        debug_visualize=False,
    )


def representative_fsr4_jobs(dx12: ModuleType, options) -> list:
    jobs = dx12.shader_jobs("balanced", "1080", False, False, options.debug_visualize)
    selected = []
    for kind in ("model-pre", "model-middle", "model-post"):
        match = next((job for job in jobs if job.kind == kind), None)
        if match is not None:
            selected.append(match)
    if len(selected) < 3:
        # Keep the smoke useful if the imported graph renames a kind: include
        # deterministic endpoints rather than silently reducing coverage.
        for job in (jobs[0], jobs[len(jobs) // 2], jobs[-1]):
            if job not in selected:
                selected.append(job)
    return selected


def compile_fsr4_jobs(
    apusr_root: Path,
    dxc: Path,
    jobs: list,
    *,
    dx12: ModuleType,
    vk: ModuleType,
    options,
    label: str,
) -> None:
    compiler_dir = dxc.parent
    base_defines = dx12.model_defines("1080", options)
    with tempfile.TemporaryDirectory(prefix="apusr_amd_fsr4_qualify_") as temp_name:
        temp = Path(temp_name)
        for index, job in enumerate(jobs):
            dxil = temp / f"{index:02}_{job.name}.dxil"
            spv = temp / f"{index:02}_{job.name}.spv"
            print(f"[qualify] FSR4 {label} DXIL {job.name}")
            dx12.compile_shader(
                apusr_root, compiler_dir, dxc, job, dxil, 180, base_defines
            )
            print(f"[qualify] FSR4 {label} SPIR-V {job.name}")
            vk.compile_shader(
                apusr_root,
                compiler_dir,
                dxc,
                job,
                spv,
                180,
                base_defines,
                "split",
            )
            print(
                f"  PASS {job.name}: dxil={dxil.stat().st_size} B "
                f"spirv={spv.stat().st_size} B"
            )


def fsr4_smoke(apusr_root: Path, dxc: Path) -> None:
    dx12, vk = fsr4_modules(apusr_root)
    options = default_fsr4_options(dx12)
    compile_fsr4_jobs(
        apusr_root,
        dxc,
        representative_fsr4_jobs(dx12, options),
        dx12=dx12,
        vk=vk,
        options=options,
        label="smoke",
    )


def fsr4_full_slice(apusr_root: Path, dxc: Path) -> None:
    dx12, vk = fsr4_modules(apusr_root)
    options = default_fsr4_options(dx12)
    jobs = dx12.shader_jobs("balanced", "1080", False, False, options.debug_visualize)
    compile_fsr4_jobs(
        apusr_root,
        dxc,
        jobs,
        dx12=dx12,
        vk=vk,
        options=options,
        label="balanced/1080",
    )


def framegen_full(apusr_root: Path, dxc: Path) -> None:
    print("[qualify] APUSR FrameGen full DX12 pack")
    with tempfile.TemporaryDirectory(prefix="apusr_amd_fg_dx12_") as temp_name:
        temp = Path(temp_name)
        require_command([
            sys.executable,
            apusr_root / "tools/compile_framegen_dx12_shaders.py",
            "--dxc", dxc,
            "--header", temp / "apusr_framegen_dxil.h",
            "--source", temp / "apusr_framegen_dxil.cpp",
        ], cwd=apusr_root)

    print("[qualify] APUSR FrameGen full Vulkan pack")
    with tempfile.TemporaryDirectory(prefix="apusr_amd_fg_vk_") as temp_name:
        temp = Path(temp_name)
        require_command([
            sys.executable,
            apusr_root / "tools/compile_framegen_vulkan_shaders.py",
            "--dxc", dxc,
            "--header", temp / "apusr_framegen_spirv.h",
            "--source", temp / "apusr_framegen_spirv.cpp",
        ], cwd=apusr_root)


def native_probe(dxc: Path, vulkan_sdk: Path | None) -> None:
    if os.name != "nt":
        raise RuntimeError("the current installed-driver native probe is Windows-only")
    pwsh = "pwsh"
    args = [
        pwsh,
        "-NoProfile",
        "-File", ROOT / "utils/amd_vulkan_isa_probe/run_windows.ps1",
        "-Dxc", dxc,
    ]
    if vulkan_sdk is not None:
        args += ["-VulkanSdk", vulkan_sdk]
    require_command(args, cwd=ROOT)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxc", help="Exact dxc executable to qualify")
    parser.add_argument("--apusr-root", type=Path)
    parser.add_argument(
        "--tier",
        choices=("inventory", "compiler", "smoke", "qualify"),
        default="compiler",
    )
    parser.add_argument(
        "--family",
        default="all",
        help="all, dot, sad, integer, fp, wave, matrix, memory, instrumentation, or a matrix id",
    )
    parser.add_argument("--native-probe", action="store_true")
    parser.add_argument("--vulkan-sdk", type=Path)
    args = parser.parse_args()

    matrix = load_matrix()
    print_matrix_status(matrix, args.family)

    apusr_root = args.apusr_root.resolve() if args.apusr_root else None
    if apusr_root is not None:
        scan_apusr(apusr_root)

    if args.tier == "inventory":
        print("APUSR AMD acceleration inventory: READY FOR ATTEMPT/QUALIFICATION")
        return 0

    if not args.dxc:
        raise RuntimeError("--dxc is required for compiler/smoke/qualify tiers")
    dxc = Path(args.dxc).resolve()
    if not dxc.is_file():
        raise RuntimeError(f"DXC executable does not exist: {dxc}")

    compiler_contracts(dxc)
    if args.tier == "compiler":
        if args.native_probe:
            native_probe(dxc, args.vulkan_sdk)
        return 0

    if apusr_root is None:
        raise RuntimeError("--apusr-root is required for smoke/qualify tiers")

    # These APUSR compilations are deliberately broad regardless of --family:
    # shader-source changes often cross family boundaries, and a compile failure
    # is cheap evidence compared with a game/hardware run. Family selection is
    # primarily for matrix/reporting now; native micro-probes remain individually
    # selectable through their dedicated tools.
    framegen_smoke(apusr_root, dxc)
    fsr4_smoke(apusr_root, dxc)

    if args.tier == "qualify":
        framegen_full(apusr_root, dxc)
        fsr4_full_slice(apusr_root, dxc)

    if args.native_probe:
        native_probe(dxc, args.vulkan_sdk)

    print(f"APUSR AMD qualification tier '{args.tier}': PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"FAIL {error}", file=sys.stderr)
        raise SystemExit(1)
