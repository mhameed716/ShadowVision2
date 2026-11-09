#!/usr/bin/env python3
"""End-to-end demo: run the real native pipeline on a generated scene.

This exists because a bug got through every gate: the no-model backend ran eight
ring-fill passes, which advance eight pixels, so a 46-px-wide post kept its
original red pixels in the middle while the API returned Ok. Nothing measured the
output pixels through the full pipeline, so nothing noticed.

Usage:
    tools/run_demo.py            # builds nothing, uses native/build/sv_g0
    tools/run_demo.py --build    # build first

Output (overwritten in docs/demo/):
    before-after.png   side-by-side, input on the left, result on the right

The raw PPM/PGM intermediates go to a temporary directory: they are 1.6 MB of
uncompressed pixels each and there is no reason to carry them in git.
"""
import argparse
import pathlib
import subprocess
import sys
import tempfile

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEMO = ROOT / "docs" / "demo"


def read_pnm(path):
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
    channels = 3 if fields[0] == b"P6" else 1
    w, h = int(fields[1]), int(fields[2])
    arr = np.frombuffer(data[i : i + w * h * channels], np.uint8)
    return arr.reshape(h, w, channels)


def write_ppm(path, arr):
    h, w, _ = arr.shape
    pathlib.Path(path).write_bytes(b"P6\n%d %d\n255\n" % (w, h) + arr.tobytes())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", action="store_true")
    args = ap.parse_args()

    if args.build:
        subprocess.run(["cmake", "-S", str(ROOT / "native"), "-B", str(ROOT / "native/build"),
                        "-DCMAKE_BUILD_TYPE=Release"], check=True, stdout=subprocess.DEVNULL)
        subprocess.run(["cmake", "--build", str(ROOT / "native/build"), "-j4"], check=True)

    binary = ROOT / "native" / "build" / "sv_g0"
    if not binary.exists():
        sys.exit("sv_g0 not built: run tools/run_demo.py --build")

    tmp = tempfile.mkdtemp(prefix="sv-demo-")
    scene = pathlib.Path(tmp) / "scene.ppm"
    mask = pathlib.Path(tmp) / "mask.pgm"
    out = pathlib.Path(tmp) / "result.ppm"
    if not scene.exists() or not mask.exists():
        from PIL import Image

        write_ppm(scene, np.array(Image.open(DEMO / "scene-input.png")))
        m = np.array(Image.open(DEMO / "mask-input.png"))
        pathlib.Path(mask).write_bytes(b"P5\n%d %d\n255\n" % (m.shape[1], m.shape[0]) + m.tobytes())

    run = subprocess.run([str(binary), "--image", str(scene), "--mask", str(mask),
                          "--out", str(out)], capture_output=True, text=True)
    print(run.stdout)
    if not out.exists():
        sys.exit(f"pipeline wrote no output\n{run.stderr}")

    a = read_pnm(scene)
    b = read_pnm(out)
    gap = np.full((a.shape[0], 6, 3), 255, np.uint8)
    from PIL import Image

    Image.fromarray(np.concatenate([a, gap, b], axis=1)).save(DEMO / "before-after.png")
    print("wrote", DEMO / "before-after.png")


if __name__ == "__main__":
    main()
