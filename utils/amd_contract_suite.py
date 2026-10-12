#!/usr/bin/env python3
"""Run all AMD compiler contract suites and report every failing suite."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

from amd_upstream_contract_audit import AMD_SUITES


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxc", required=True)
    parser.add_argument(
        "--only", action="append", choices=AMD_SUITES,
        help="Run one named suite; can be specified more than once",
    )
    args = parser.parse_args()
    suites = args.only or AMD_SUITES
    dxc = str(Path(args.dxc).resolve())
    root = Path(__file__).resolve().parent
    failures: list[str] = []

    for name in suites:
        print(f"\n--- AMD compiler contract: {name} ---", flush=True)
        proc = subprocess.run(
            [sys.executable, str(root / name), "--dxc", dxc],
            text=True, encoding="utf-8", errors="replace",
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
        )
        if proc.stdout:
            print(proc.stdout, end="" if proc.stdout.endswith("\n") else "\n")
        if proc.stderr:
            print(
                proc.stderr, file=sys.stderr,
                end="" if proc.stderr.endswith("\n") else "\n",
            )
        if proc.returncode == 0:
            print(f"PASS {name}", flush=True)
        else:
            failures.append(name)
            print(f"FAIL {name} (exit {proc.returncode})", file=sys.stderr, flush=True)

    print(f"\nAMD contract suites: {len(suites) - len(failures)}/{len(suites)} passed")
    if failures:
        print("Failing suites: " + ", ".join(failures), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
