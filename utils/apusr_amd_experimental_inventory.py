#!/usr/bin/env python3
"""Validate/report the APUSR empirical AMD candidate pool."""

from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
POOL = ROOT / "utils/apusr_amd_experimental_candidates.json"
VALID = {"sprayable", "implemented-search", "ingress-blocked", "deferred-hardware"}


def main() -> int:
    data = json.loads(POOL.read_text(encoding="utf-8"))
    rows = data.get("candidates")
    if not isinstance(rows, list) or not rows:
        raise RuntimeError("experimental candidate pool is empty")

    seen: set[str] = set()
    counts = {status: 0 for status in VALID}
    for row in rows:
        candidate_id = row.get("id")
        if not candidate_id or candidate_id in seen:
            raise RuntimeError(f"invalid/duplicate candidate id: {candidate_id!r}")
        seen.add(candidate_id)
        status = row.get("status")
        if status not in VALID:
            raise RuntimeError(f"{candidate_id}: invalid status {status!r}")
        counts[status] += 1
        for field in ("consumer", "target", "search", "probe", "oracle"):
            if not row.get(field):
                raise RuntimeError(f"{candidate_id}: missing {field}")
        if status in {"ingress-blocked", "deferred-hardware"} and not row.get("reason"):
            raise RuntimeError(f"{candidate_id}: {status} row requires a reason")
        if status == "sprayable" and "pending" not in str(row.get("probe", "")).lower():
            raise RuntimeError(
                f"{candidate_id}: sprayable row must make its pending probe explicit"
            )

    width = max(len(row["id"]) for row in rows)
    for row in rows:
        print(f"[experimental] {row['id']:<{width}}  {row['status']}")
    print(
        "experimental pool: "
        + ", ".join(f"{status}={counts[status]}" for status in sorted(counts))
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"FAIL {error}")
        raise SystemExit(1)
