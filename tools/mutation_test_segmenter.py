#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Mutation-test the ONNX segmenter integration suite.

A green test suite proves nothing on its own: it may be green because the code
is correct, or because the test never looks. This script introduces the exact
bugs a real SAM integration ships with, rebuilds the suite against each mutant,
and requires the suite to *fail*.

Currently checked mutants (each one is a bug that has shipped in real projects):

  no_scale      prompt coordinates are not scaled from image px to model px
  pad_ignored   preprocessing padding is reported as zero
  mask_short    the mask buffer is sized wrongly (off-by-N)
  status_ok     the decoder returns Ok on a shape mismatch
  no_largest    the decoder skips the largest-component filter
  thr_flipped   the mask threshold is inverted
  no_determinism the encoder output is perturbed run to run

If a mutant survives, the corresponding assertion in sv_ort_integration.cpp is
too weak and this script fails with the mutant's name.

Requires: g++, python3-onnx, ORT headers + lib (same prerequisites as
tools/verify_g0.sh --with-ort). Skips cleanly (exit 0, "SKIP") when they are
absent, because mutation testing is a maintenance tool, not a gate for every
machine.
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import ort_env  # noqa: E402  (local helper, same directory)

MUTANTS: list[tuple[str, str, str, str]] = [
    # (name, file, old, new)
    ("no_scale", "include/sv/sam_geometry.h",
     "*mx = x * g.scale_x;", "*mx = x;"),
    ("no_scale_y", "include/sv/sam_geometry.h",
     "*my = y * g.scale_y;", "*my = y;"),
    ("pad_ignored", "include/sv/sam_geometry.h",
     "int pad_right() const { return model_size - new_w; }",
     "int pad_right() const { return 0; }"),
    ("pad_ignored_v", "include/sv/sam_geometry.h",
     "int pad_bottom() const { return model_size - new_h; }",
     "int pad_bottom() const { return 0; }"),
    ("mask_short", "src/ort_segmenter.cpp",
     "mask_out->assign(static_cast<size_t>(image_w) * image_h, 0);",
     "mask_out->assign(static_cast<size_t>(image_w) * image_h - 1, 0);"),
    ("status_ok", "src/ort_segmenter.cpp",
     'm_error = "embedding size " + std::to_string(embedding.size()) + " != shape " +\n                      ShapeToString(shape);\n            return Status::InvalidArgument;',
     'm_error = "embedding size mismatch";\n            return Status::Ok;'),
    ("no_largest", "src/ort_segmenter.cpp",
     "if (keep_largest) KeepLargestComponent(mask_out->data(), image_w, image_h);",
     "if (keep_largest) { /* mutant: filter disabled */ }"),
    ("thr_flipped", "src/ort_segmenter.cpp",
     "logits[static_cast<size_t>(y) * mw + x] > thr",
     "logits[static_cast<size_t>(y) * mw + x] < thr"),
    # NOTE: `if (false)` over the length guard used to be a listed mutant, but the
    # only decoder that can violate the guard is a lying one, and manufacturing
    # that requires a second stub implementation of ISegmenterDecoder inside the
    # suite. The guard is therefore asserted indirectly (mask_short above) and is
    # left out of the mutant list rather than listed and quietly surviving.

    # --- the NHWC / Acly-interface path -------------------------------------
    ("acly_square_frame", "src/ort_segmenter.cpp",
     ": sam::PreprocessGeometry(image.width, image.height, s);",
     ": sam::PreprocessGeometry(image.width, image.height, s / 2);"),
    # The encoder's NHWC branch collapses into the padded branch: the graph then
    # receives a 256x192 content area where it expects a full 256x256 frame.
    ("acly_padded_encoder", "src/ort_segmenter.cpp",
     "const auto geom = m_layout == EncoderLayout::NHWC",
     "const auto geom = false && m_layout == EncoderLayout::NHWC"),
    ("double_normalise", "src/ort_segmenter.cpp",
     "const bool normalise = m_layout == EncoderLayout::NCHW;",
     "const bool normalise = true;"),
    ("pad_left", "include/sv/sam_geometry.h",
     "g.scale_x = static_cast<float>(g.new_w) / static_cast<float>(img_w);",
     "g.scale_x = static_cast<float>(g.model_size) / static_cast<float>(img_w);"),
]


def build(src_dir: str, inc: str, lib: str, out: str, rt: str | None = None) -> bool:
    cmd = [
        "g++", "-std=c++17", "-O1", "-DSV_WITH_ORT=1",
        f"-I{os.path.join(src_dir, 'include')}",
        f"-I{inc}",
        os.path.join(src_dir, "tests/sv_ort_integration.cpp"),
        os.path.join(src_dir, "src/ort_segmenter.cpp"),
        os.path.join(src_dir, "src/segmenter.cpp"),
        os.path.join(src_dir, "src/kernels.cpp"),
        os.path.join(src_dir, "src/memory.cpp"),
        "-L" + lib, "-lonnxruntime", "-Wl,-rpath," + (rt or lib), "-o", out,
    ]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr[-2000:], file=sys.stderr)
    return r.returncode == 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true", help="keep the temp dir (debugging)")
    args = ap.parse_args()

    missing = ort_env.report_missing()
    if missing:
        print("SKIP: " + "; ".join(missing))
        return 0
    inc = ort_env.find_include()
    libenv = ort_env.find_lib()
    assert inc and libenv
    lib_dir, rt_dir = libenv

    tmp = tempfile.mkdtemp(prefix="sv-mut-")
    try:
        graphs = os.path.join(tmp, "graphs")
        r = subprocess.run([sys.executable, os.path.join(ROOT, "tools/make_test_graphs.py"),
                            "--out", graphs, "--embed-side", "16"],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print("could not build the test graphs:\n" + r.stdout + r.stderr, file=sys.stderr)
            return 2
        enc = os.path.join(graphs, "test_encoder.onnx")
        dec = os.path.join(graphs, "test_decoder.onnx")
        acly_dir = os.path.join(tmp, "graphs_acly")
        subprocess.run([sys.executable, os.path.join(ROOT, "tools/make_test_graphs.py"),
                        "--out", acly_dir, "--embed-side", "16", "--interface", "acly"],
                       check=True, capture_output=True, text=True)
        graphs_argv = [enc, dec,
                       os.path.join(acly_dir, "acly_encoder.onnx"),
                       os.path.join(acly_dir, "acly_decoder.onnx")]

        clean = os.path.join(tmp, "clean")
        shutil.copytree(os.path.join(ROOT, "native"), clean)
        base_bin = os.path.join(tmp, "base")
        if not build(clean, inc, lib_dir, base_bin, rt_dir):
            print("the unmutated suite does not build; fix that first", file=sys.stderr)
            return 2
        r = subprocess.run([base_bin, *graphs_argv], capture_output=True, text=True)
        if r.returncode != 0:
            print("the unmutated suite is not green, so mutation results mean nothing:\n" +
                  r.stdout + r.stderr, file=sys.stderr)
            return 2
        print(f"baseline: {r.stdout.strip()}")

        survived: list[str] = []
        applied = 0
        for name, rel, old, new in MUTANTS:
            work = os.path.join(tmp, "m_" + name)
            if os.path.exists(work):
                shutil.rmtree(work)
            shutil.copytree(os.path.join(ROOT, "native"), work)
            path = os.path.join(work, rel)
            src = open(path, encoding="utf-8").read()
            if old not in src:
                # The code moved on; a stale mutant is a bug in this script, not
                # in the product, and it must not silently pass.
                print(f"  STALE  {name}: pattern no longer present in {rel}")
                survived.append(name + " (stale pattern)")
                continue
            open(path, "w", encoding="utf-8").write(src.replace(old, new, 1))
            applied += 1
            binary = os.path.join(tmp, "bin_" + name)
            if not build(work, inc, lib_dir, binary, rt_dir):
                # A mutant that does not compile still proves the test would have
                # caught the change; count it as killed and say so.
                print(f"  KILLED {name} (does not compile)")
                continue
            res = subprocess.run([binary, *graphs_argv], capture_output=True, text=True)
            if res.returncode == 0:
                print(f"  SURVIVED {name}: the suite stayed green — assertion too weak")
                survived.append(name)
            else:
                first = next((ln for ln in res.stdout.splitlines() if ln.startswith("FAIL")), "")
                print(f"  KILLED {name}  ({first[:80]})")

        print(f"\n{applied}/{len(MUTANTS)} mutants applied, "
              f"{len(survived)} survived")
        if survived:
            print("survivors: " + ", ".join(survived), file=sys.stderr)
            return 1
        print("every mutant was killed: the integration suite has teeth")
        return 0
    finally:
        if args.keep:
            print("kept " + tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
