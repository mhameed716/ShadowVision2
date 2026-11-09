#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Locate a usable ONNX Runtime (headers + linkable library) on this machine.

One implementation, three callers (the gate, the integration runner, the mutation
tester). Duplicating this logic is how you end up with one gate that finds ORT
and another that silently reports "skipped" — which then reads as green.

Discovery order:
  1. SV_ORT_INCLUDE / SV_ORT_LIB / SV_ORT_RUNTIME environment variables,
  2. the pip wheel (headers are *not* shipped there, so headers must come from
     an explicit include dir or the /tmp/ortinc cache used during development).

Nothing is written outside a temp dir.
"""
from __future__ import annotations

import glob
import importlib.util
import os
import tempfile

DEFAULT_INCLUDE_GUESSES = (
    "/tmp/ortinc/onnxruntime/core/session",
    "/usr/local/include/onnxruntime/core/session",
    "/usr/include/onnxruntime/core/session",
)


def _forced_missing() -> bool:
    """Test hook: pretend ORT is absent so the failure path can be verified.

    A gate whose "missing prerequisite" branch is never executed is a gate you
    are trusting, not testing. verify_g0.sh sets this and requires a non-zero
    exit.
    """
    return os.environ.get("SV_ORT_FORCE_MISSING") == "1"


def find_include() -> str | None:
    if _forced_missing():
        return None
    env = os.environ.get("SV_ORT_INCLUDE")
    if env and os.path.isfile(os.path.join(env, "onnxruntime_cxx_api.h")):
        return env
    for guess in DEFAULT_INCLUDE_GUESSES:
        if os.path.isfile(os.path.join(guess, "onnxruntime_cxx_api.h")):
            return guess
    return None


def find_lib() -> tuple[str, str] | None:
    """Return (link_dir, runtime_dir) such that -L<link_dir> -lonnxruntime works.

    The pip wheel ships only the versioned soname (`libonnxruntime.so.1.23.2`).
    The linker needs `libonnxruntime.so` and the loader needs the soname
    `libonnxruntime.so.1`, so both symlinks are created in one temp dir, which is
    then used for -L *and* for RPATH.
    """
    if _forced_missing():
        return None
    env_lib = os.environ.get("SV_ORT_LIB")
    env_rt = os.environ.get("SV_ORT_RUNTIME", env_lib)
    if env_lib and os.path.isdir(env_lib):
        if glob.glob(os.path.join(env_lib, "libonnxruntime.so*")):
            return env_lib, env_rt or env_lib
    try:
        spec = importlib.util.find_spec("onnxruntime")
        if not spec or not spec.origin:
            return None
        capi = os.path.join(os.path.dirname(spec.origin), "capi")
        libs = sorted(glob.glob(os.path.join(capi, "libonnxruntime.so*")))
        if not libs:
            return None
        versioned = os.path.abspath(libs[-1])
        linkdir = tempfile.mkdtemp(prefix="sv-ortlib-")
        os.symlink(versioned, os.path.join(linkdir, "libonnxruntime.so"))
        soname = os.path.basename(versioned).split(".so")[0] + ".so.1"
        os.symlink(versioned, os.path.join(linkdir, soname))
        return linkdir, linkdir
    except Exception:  # noqa: BLE001 - absent is absent
        return None


def have_python_onnx() -> bool:
    if _forced_missing():
        return False
    try:
        import onnx  # noqa: F401
        return True
    except ImportError:
        return False


def report_missing() -> list[str]:
    """Human-readable list of what is missing (empty when everything is present)."""
    missing = []
    if not find_include():
        missing.append("ORT headers (set SV_ORT_INCLUDE to the dir holding "
                       "onnxruntime_cxx_api.h)")
    if not find_lib():
        missing.append("libonnxruntime (set SV_ORT_LIB, or pip install onnxruntime)")
    if not have_python_onnx():
        missing.append("python package 'onnx' (pip install onnx)")
    return missing


def _main() -> int:
    """CLI: `python3 tools/ort_env.py --check` exits 1 when ORT is incomplete.

    A gate that prints "SKIP" and exits 0 is worse than no gate: it reads as
    green in a log. --with-ort calls this first so a missing prerequisite fails
    loudly instead of quietly reducing coverage.
    """
    import sys

    missing = report_missing()
    if missing:
        for m in missing:
            print("missing: " + m, file=sys.stderr)
        return 1
    print(f"ORT headers: {find_include()}")
    lib = find_lib()
    print(f"ORT library: {lib[1] if lib else '?'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(_main())
