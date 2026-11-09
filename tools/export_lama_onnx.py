#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Export LaMa (dilated) to ONNX for ShadowVision2, with the exact contract the
native backend expects.

Run on a desktop (NOT on the phone):

    pip install torch torchvision onnx onnxruntime numpy
    python tools/export_lama_onnx.py --ckpt big-lama.pt --out models/lama_fp32.onnx
    python tools/export_lama_onnx.py --ckpt big-lama.pt --out models/lama_fp16.onnx --fp16

Why the script exists at all: the ONNX export is where mobile projects usually
lose a week. Two choices here are deliberate and worth keeping:

  * fixed 512x512 trace shape (dynamic axes cost the QNN backend every chance of
    running, and a phone runs one crop size anyway),
  * TWO inputs (image, mask) rather than one pre-masked tensor, so the C++ side
    can keep the original pixels for the blend and for the colour correction.

The output is named "output" and is RGB in 0..1, NCHW.
"""
from __future__ import annotations

import argparse
import sys


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ckpt", required=True, help="big-lama.pt (or a diluted checkpoints dir)")
    ap.add_argument("--out", required=True, help="output .onnx path")
    ap.add_argument("--size", type=int, default=512, help="fixed square size (default 512)")
    ap.add_argument("--fp16", action="store_true", help="convert weights to float16")
    args = ap.parse_args()

    try:
        import torch
        from saicinpainting.training.trainers import load_checkpoint
    except Exception as e:  # pragma: no cover - desktop only
        print(f"missing dependency: {e}\n"
              "install: pip install torch && pip install "
              "git+https://github.com/advimman/lama.git", file=sys.stderr)
        return 2

    model, _ = load_checkpoint(args.ckpt, location="cpu")
    model.eval()

    s = args.size
    img = torch.rand(1, 3, s, s)
    mask = torch.zeros(1, 1, s, s)
    mask[:, :, s // 4 : s // 2, s // 4 : s // 2] = 1.0
    img_masked = img * (1 - mask)  # LaMa's own convention internally

    class ExportWrapper(torch.nn.Module):
        """Expose (image, mask) -> inpainted image, multiplying inside the graph so
        the C++ caller never has to replicate the model's masking convention."""

        def __init__(self, m):
            super().__init__()
            self.m = m

        def forward(self, image, mask):  # noqa: A002 - matches the ONNX input name
            return self.m(image * (1 - mask), mask)

    wrapper = ExportWrapper(model).eval()

    torch.onnx.export(
        wrapper,
        (img, mask),
        args.out,
        input_names=["image", "mask"],
        output_names=["output"],
        opset_version=17,
        do_constant_folding=True,
        dynamic_axes=None,  # fixed shapes on purpose: the NPU path needs them
    )
    print(f"wrote {args.out} (fp32, {s}x{s})")

    if args.fp16:
        import onnx
        from onnxconverter_common import float16

        m = onnx.load(args.out)
        m16 = float16.convert_float_to_float16(m, keep_io_types=True)
        out16 = args.out.replace(".onnx", "_fp16.onnx")
        onnx.save(m16, out16)
        print(f"wrote {out16} (fp16 weights, fp32 io)")

    # Sanity: the graph must have exactly the names the C++ backend hard-codes.
    import onnx

    m = onnx.load(args.out)
    names = [i.name for i in m.graph.input]
    print("inputs :", names)
    print("outputs:", [o.name for o in m.graph.output])
    if names != ["image", "mask"]:
        print("WARNING: input names differ from sv::kImageName/kMaskName — update "
              "native/src/ort_backend.cpp or re-export with the expected names.",
              file=sys.stderr)
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
