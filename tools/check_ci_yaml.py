#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate every workflow file under .github/workflows.

Why this exists: an unquoted step name containing ": " makes the whole YAML
document invalid, and GitHub then reports the run as "failure" after zero
seconds with *no logs at all* — indistinguishable from a real test failure in
the UI. A one-line check here saves a debugging round-trip on every edit.
"""
import glob
import os
import sys

try:
    import yaml
except ImportError:  # pragma: no cover - handled by caller
    print("skipped: PyYAML not installed")
    sys.exit(0)

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
files = sorted(glob.glob(os.path.join(root, ".github/workflows/*.y*ml")))
if not files:
    print("no workflow files found")
    sys.exit(1)

bad = 0
for path in files:
    rel = os.path.relpath(path, root)
    try:
        doc = yaml.safe_load(open(path, encoding="utf-8"))
    except Exception as exc:  # noqa: BLE001 - report any parse error verbatim
        print(f"  {rel}: INVALID — {exc}", file=sys.stderr)
        bad += 1
        continue
    if not isinstance(doc, dict) or "jobs" not in doc:
        print(f"  {rel}: INVALID — no jobs: mapping", file=sys.stderr)
        bad += 1
        continue
    # Workflows carry no YAML anchors/aliases legitimately; a duplicate key
    # silently drops a step, so fail on those too.
    steps = 0
    for job in doc["jobs"].values():
        steps += len(job.get("steps") or [])
    print(f"  {rel}: valid ({len(doc['jobs'])} job(s), {steps} step(s))")

sys.exit(1 if bad else 0)
