#!/usr/bin/env python3
"""Pixel-level acceptance check on a real scene, run inside verify_g0.sh.

Why this file exists: the fill bug that shipped was invisible to every assertion
in the suite. Each stage returned Ok, the timings were fine, the mask was valid —
and the object was still in the picture. The only thing that catches that class of
defect is looking at the output pixels and asking "is the object gone?".

The scene is generated here, not taken from a fixture, so the check cannot rot
into "we compare against the output we produced last time".

Usage: check_demo_output.py /path/to/sv_g0
"""
import pathlib
import subprocess
import sys
import tempfile

import numpy as np

W, H = 400, 300
POST_X0, POST_X1 = 150, 200        # 50 px wide: far wider than an 8-px ring fill
POST_Y0, POST_Y1 = 80, 260
POST_RGB = (200, 45, 40)
SKY_TOP = np.array([90, 140, 205])
SKY_BOTTOM = np.array([170, 200, 235])


def make_scene():
    """Sky gradient + a red post + a thin cable, so both thin and wide objects are
    covered by one run."""
    yy = np.arange(H)[:, None]
    sky = (SKY_TOP + (SKY_BOTTOM - SKY_TOP) * (yy / (H - 1))).astype(np.uint8)
    img = np.repeat(sky[:, None, :], W, axis=1).astype(np.uint8)
    mask = np.zeros((H, W), np.uint8)

    img[POST_Y0:POST_Y1, POST_X0:POST_X1] = POST_RGB
    mask[POST_Y0:POST_Y1, POST_X0:POST_X1] = 255

    # Thin diagonal cable from the post to the right edge.
    for x in range(60, 380):
        y = int(150 + 12 * np.sin((x - 60) / 55.0))
        img[y : y + 2, x] = (20, 20, 25)
        mask[max(0, y - 1) : y + 3, x] = 255
    return img, mask


def write_ppm(path, arr):
    h, w, _ = arr.shape
    pathlib.Path(path).write_bytes(b"P6\n%d %d\n255\n" % (w, h) + arr.tobytes())


def write_pgm(path, arr):
    h, w = arr.shape
    pathlib.Path(path).write_bytes(b"P5\n%d %d\n255\n" % (w, h) + arr.tobytes())


def read_ppm(path):
    data = pathlib.Path(path).read_bytes()
    fields, i = [], 0
    while len(fields) < 4:
        while data[i : i + 1].isspace():
            i += 1
        j = i
        while not data[j : j + 1].isspace():
            j += 1
        fields.append(data[i:j])
        i = j
    i += 1
    w, h = int(fields[1]), int(fields[2])
    arr = np.frombuffer(data[i : i + w * h * 3], np.uint8)
    return arr.reshape(h, w, 3)


def main():
    binary = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "native/build/sv_g0")
    if not binary.exists():
        sys.exit(f"check_demo_output: {binary} missing")

    failures = 0
    checks = 0

    def check(ok, label, detail=""):
        nonlocal failures, checks
        checks += 1
        print(f"  [{'PASS' if ok else 'FAIL'}] {label}" + (f"  {detail}" if detail else ""))
        if not ok:
            failures += 1

    img, mask = make_scene()
    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        write_ppm(td / "scene.ppm", img)
        write_pgm(td / "mask.pgm", mask)
        run = subprocess.run(
            [str(binary), "--image", str(td / "scene.ppm"), "--mask", str(td / "mask.pgm"),
             "--backend", "synthetic", "--out", str(td / "out.ppm")],
            capture_output=True, text=True)
        if not (td / "out.ppm").exists():
            sys.exit(f"check_demo_output: pipeline produced no output\n{run.stdout}{run.stderr}")
        out = read_ppm(td / "out.ppm")

    check(run.returncode == 0, "pipeline exits 0")

    # 1. The wide object must not survive anywhere.
    interior = out[POST_Y0 + 3 : POST_Y1 - 3, POST_X0 + 3 : POST_X1 - 3].astype(int)
    still_red = int(((interior[:, :, 0] > 150) & (interior[:, :, 2] < 110)).sum())
    check(still_red == 0, "wide object fully removed",
          f"{still_red} red px left of {interior.shape[0] * interior.shape[1]}")

    # 2. ...and what replaced it must be sky, not a flat block or the original.
    col = W // 2  # through the middle of the post
    inside = out[POST_Y0 + 10 : POST_Y1 - 10, col].astype(int)
    # Sky down a column is a smooth gradient: adjacent rows differ by ~0-1.
    steps = np.abs(np.diff(inside[:, 1]))
    check(int(steps.max()) <= 2, "fill follows the sky gradient", f"max row step {int(steps.max())}")
    # Compare with the real sky immediately left of the post on the same rows.
    ref = out[POST_Y0 + 10 : POST_Y1 - 10, POST_X0 - 20].astype(int)
    err = np.abs(inside - ref).max()
    check(int(err) <= 12, "fill matches neighbouring sky", f"max channel error {int(err)}")

    # 2b. No red halo around the object. Blending with the raw stroke instead of
    #     the dilated mask leaves exactly these: the object's own edge pixels.
    ring = np.zeros((H, W), bool)
    ring[POST_Y0 - 4 : POST_Y1 + 4, POST_X0 - 4 : POST_X1 + 4] = True
    ring[POST_Y0:POST_Y1, POST_X0:POST_X1] = False
    rim = out[ring].astype(int)
    rim_red = int(((rim[:, 0] > 150) & (rim[:, 2] < 110) & (rim[:, 0] - rim[:, 2] > 60)).sum())
    check(rim_red == 0, "no red halo around the object", f"{rim_red} rim px")

    # 3. The thin cable must be gone too (0 changed outside the halo check lives in
    #    the C++ suite; this one is about the object being visible).
    cable_rows = range(130, 175)
    cable_dark = int((out[list(cable_rows), 250:350].astype(int).sum(axis=2) < 150).sum())
    check(cable_dark == 0, "thin cable removed", f"{cable_dark} dark px remain")

    # 4. Nothing outside the mask may change (the documented halo is checked in C++).
    outside = mask == 0
    diff = np.abs(out.astype(int) - img.astype(int)).sum(axis=2)
    changed_outside = int(((diff > 6) & outside).sum())
    check(changed_outside * 100 < int(outside.sum()), "changes stay near the mask",
          f"{changed_outside} px of {int(outside.sum())}")

    print(f"check_demo_output: {checks} checks, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
