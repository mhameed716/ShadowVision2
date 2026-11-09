#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build and run the ONNX segmenter integration test on this machine.

Wraps the three steps that are easy to get wrong by hand:
  1. generate contract-shaped test graphs (tools/make_test_graphs.py),
  2. find the ONNX Runtime headers *and* a linkable library,
  3. compile the test against the real product sources and run it.

Everything is done into a temp dir; nothing in the repo is modified and no
weights are needed. If ONNX Runtime is not available this exits 0 with "SKIP"
when --allow-skip is passed (CI on a machine without ORT), and exits 2
otherwise — a gate must not silently pass because a prerequisite is missing.

Environment:
  SV_ORT_INCLUDE  dir containing onnxruntime_cxx_api.h
  SV_ORT_LIB      dir containing libonnxruntime.so (+.so.1 devname)
  SV_ORT_RUNTIME  dir with the versioned .so, if different from SV_ORT_LIB
"""
from __future__ import annotations

import argparse
import glob
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import ort_env  # noqa: E402
SOURCES = [
    "tests/sv_ort_integration.cpp",
    "src/ort_segmenter.cpp",
    "src/segmenter.cpp",
    "src/kernels.cpp",
    "src/memory.cpp",
]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--allow-skip", action="store_true",
                    help="exit 0 with SKIP when ONNX Runtime is unavailable")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--asan", action="store_true",
                    help="build the test with ASan/UBSan as well")
    args = ap.parse_args()

    missing = ort_env.report_missing()
    if missing:
        msg = "SKIP: " + "; ".join(missing)
        if args.allow_skip:
            print(msg + " — integration test not run")
            return 0
        print(msg, file=sys.stderr)
        return 2

    inc = ort_env.find_include()
    lib = ort_env.find_lib()
    assert inc and lib, "report_missing() said everything was present"

    tmp = tempfile.mkdtemp(prefix="sv-ortint-")
    try:
        graphs = os.path.join(tmp, "graphs")
        subprocess.run([sys.executable, os.path.join(ROOT, "tools/make_test_graphs.py"),
                        "--out", graphs, "--embed-side", "16"],
                       check=True, capture_output=True, text=True)

        lib_dir, rt_dir = lib
        binary = os.path.join(tmp, "sv_ort_integration")
        san = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.asan else []
        cmd = ["g++", "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-DSV_WITH_ORT=1",
               *san,
               "-I" + os.path.join(ROOT, "native", "include"), "-I" + inc]
        cmd += [os.path.join(ROOT, "native", s) for s in SOURCES]
        cmd += [*san, "-L" + lib_dir, "-lonnxruntime", "-Wl,-rpath," + rt_dir, "-o", binary]
        build = subprocess.run(cmd, capture_output=True, text=True)
        if build.returncode != 0:
            print("integration test does not compile:\n" + build.stderr[-3000:],
                  file=sys.stderr)
            return 2

        run = subprocess.run([binary, os.path.join(graphs, "test_encoder.onnx"),
                              os.path.join(graphs, "test_decoder.onnx")],
                             capture_output=True, text=True)
        print(run.stdout.strip())
        if run.returncode != 0:
            print(run.stderr[-2000:], file=sys.stderr)
            return 1
        return 0
    finally:
        if args.keep:
            print("kept " + tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
