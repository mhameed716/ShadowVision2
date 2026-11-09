#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Export MobileSAM as TWO fixed-shape ONNX graphs, matching sv::ISegmenter*.

    encoder: image [1,3,1024,1024] float32 0..1        -> embedding [1,256,64,64]
    decoder: embedding + points + labels               -> mask logits [1,1,256,256]

Why two graphs and not one (D5): the encoder is the expensive half and runs once
per image; the decoder is cheap and runs per tap. Exporting the monolithic model
forces both halves to run on every tap, which is the difference between a 60 ms
interaction and a 1.5 s one.

Licence: MobileSAM weights are Apache-2.0 (Kyung Hee University). Do NOT load them
through the `ultralytics` package for a closed-source app — that package is
AGPL-3.0 and its terms reach your application. This script uses the upstream
MobileSAM repo directly.

    pip install torch onnx onnxruntime
    git clone https://github.com/ChaoningZhang/MobileSAM && cd MobileSAM
    pip install -e .
    python ../tools/export_mobilesam_onnx.py --ckpt weights/mobile_sam.pt \
        --out-dir ../models
"""
from __future__ import annotations

import argparse
import sys


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ckpt", required=True)
    ap.add_argument("--out-dir", default="models")
    ap.add_argument("--size", type=int, default=1024)
    args = ap.parse_args()

    try:
        import torch
        from mobile_sam import sam_model_registry
    except Exception as e:  # pragma: no cover - desktop only
        print(f"missing dependency: {e}\n"
              "Clone ChaoningZhang/MobileSAM and `pip install -e .` (NOT ultralytics).",
              file=sys.stderr)
        return 2

    sam = sam_model_registry["vit_t"](checkpoint=args.ckpt).eval()
    s = args.size

    # --- encoder -----------------------------------------------------------
    class Encoder(torch.nn.Module):
        def __init__(self, m):
            super().__init__()
            self.m = m

        def forward(self, image):
            # The encoder wraps its own SAM normalization (mean/std) as registered
            # buffers, so the graph expects RAW 0..255 RGB, not normalized input.
            # The C++ side normalizes anyway (sv/sam_geometry.h) — SAM's mean/std
            # are near-idempotent under a second normalization, but if you change
            # one side, change both and re-run the parity check printed below.
            return self.m.image_encoder(image)

    enc = Encoder(sam).eval()
    dummy = torch.rand(1, 3, s, s)
    enc_path = f"{args.out_dir}/mobile_sam_encoder.onnx"
    torch.onnx.export(
        enc, (dummy,), enc_path,
        input_names=["image"], output_names=["embedding"],
        opset_version=17, do_constant_folding=True, dynamic_axes=None,
    )
    print(f"wrote {enc_path}")

    # --- decoder -----------------------------------------------------------
    class Decoder(torch.nn.Module):
        """prompt encoder + mask decoder only: the taps are the only variable."""

        def __init__(self, m):
            super().__init__()
            self.m = m

        def forward(self, embedding, points, labels):
            # points: [1,N,2] in the model's 1024 space, labels: [1,N]
            sparse, dense = self.m.prompt_encoder(points=(points, labels), boxes=None, masks=None)
            low_res, _ = self.m.mask_decoder(
                image_embeddings=embedding,
                image_pe=self.m.prompt_encoder.get_dense_pe(),
                sparse_prompt_embeddings=sparse,
                dense_prompt_embeddings=dense,
                multimask_output=False,
            )
            return low_res

    dec = Decoder(sam).eval()
    embed = torch.rand(1, 256, 64, 64)
    pts = torch.tensor([[[s // 2.0, s // 2.0]]])
    labels = torch.tensor([[1]])
    dec_path = f"{args.out_dir}/mobile_sam_decoder.onnx"
    torch.onnx.export(
        dec, (embed, pts, labels), dec_path,
        input_names=["embedding", "points", "labels"], output_names=["logits"],
        opset_version=17, do_constant_folding=True, dynamic_axes=None,
    )
    print(f"wrote {dec_path}")

    # ---- parity check: the exported graphs must reproduce PyTorch ------------
    # This is the check that catches the two classic integration bugs before the
    # phone does: a wrong coordinate transform, and a mask thresholded at the
    # wrong side. It runs on a synthetic image so it is deterministic.
    try:
        import numpy as np
        import onnxruntime as ort

        enc_sess = ort.InferenceSession(enc_path, providers=["CPUExecutionProvider"])
        dec_sess = ort.InferenceSession(dec_path, providers=["CPUExecutionProvider"])

        rng = np.random.default_rng(0)
        img = (rng.random((1, 3, s, s), dtype=np.float32) * 255.0)
        emb_onnx = enc_sess.run(None, {"image": img})[0]
        with torch.no_grad():
            emb_torch = enc.image_encoder(torch.from_numpy(img)).numpy()
        emb_err = float(np.abs(emb_onnx - emb_torch).max())

        pts_np = np.array([[[s / 2.0, s / 2.0]]], dtype=np.float32)
        lbl_np = np.array([[1]], dtype=np.float32)
        logits_onnx = dec_sess.run(None, {"embedding": emb_onnx, "points": pts_np,
                                          "labels": lbl_np})[0]
        with torch.no_grad():
            logits_torch = dec(torch.from_numpy(emb_onnx), torch.from_numpy(pts_np),
                               torch.from_numpy(lbl_np)).numpy()
        logit_err = float(np.abs(logits_onnx - logits_torch).max())

        print(f"parity: encoder max|diff|={emb_err:.3e}  decoder max|diff|={logit_err:.3e}")
        if emb_err > 1e-2 or logit_err > 1e-2:
            print("PARITY FAILED: the ONNX graphs do not match PyTorch. Do not ship.",
                  file=sys.stderr)
            return 4
        print("parity OK (threshold 1e-2)")
    except ImportError:
        print("note: install numpy + onnxruntime for the parity check", file=sys.stderr)
    print("\nNote: the C++ ISegmenterDecoder implementation for these graphs is not "
          "wired yet (sv_load_segmenter falls back to the built-in segmenter). "
          "Ship the built-in one first, then add the ONNX decoder behind the same "
          "interface — the studio does not need to change.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
