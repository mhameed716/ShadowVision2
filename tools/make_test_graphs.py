#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build two *real ONNX graphs* that satisfy the MobileSAM I/O contract.

These are not the SAM weights — they are tiny, deterministic graphs with the
same input names, shapes and dtypes as the export in
tools/export_mobilesam_onnx.py. Their only job is to let a machine with no
MobileSAM checkpoint (CI, a laptop, this sandbox) run the *actual* C++ path:

    encode() -> Ort::Session::Run -> decode() -> mask in image pixels

Why bother when the geometry is already unit-tested? Because the bugs that
survive unit tests are the ones at the seams: a tensor whose shape disagrees
with the buffer we bound to it, an input listed in the wrong order, a mask
indexed before its Status is checked, a decoder that silently reads the wrong
stride. None of those are visible to `-fsyntax-only`, and all of them are the
kind of thing a device build surfaces at 2 a.m.

Contract produced (identical to the export script):
    encoder: image     float32 [1,3,S,S]   -> embedding float32 [1,256,S/16,S/16]
    decoder: embedding float32 [1,256,H,W] + points float32 [1,N,2]
             + labels float32 [1,N]         -> logits    float32 [1,1,H*4,W*4]

The graphs are *shaped like SAM* and *behave like an oracle*: given a point,
they emit a positive disc of radius R centered on that point in model space.
So the integration test can assert something much stronger than "no crash" —
it can assert that a tap at image pixel (x,y) produces a mask centred on (x,y),
which is exactly the coordinate-transform bug class.

Usage:
    python3 tools/make_test_graphs.py --out /tmp/segtest [--embed-side 64]
"""
from __future__ import annotations

import argparse
import os


def build(out_dir: str, embed_side: int = 64, radius: float = 40.0) -> tuple[str, str]:
    import onnx
    from onnx import TensorProto, helper, numpy_helper

    os.makedirs(out_dir, exist_ok=True)
    model_size = embed_side * 16
    mask_side = embed_side * 4
    n_points = 4

    # ---------------- encoder -------------------------------------------------
    # image -> embedding. The real encoder is a ViT; the contract is only
    # "NCHW float32 in, [1,C,h,w] float32 out", so a deterministic linear
    # reduction is enough: mean over each 16x16 block, per channel, replicated.
    # It must be shape-faithful because the C++ derives the mask side from it.
    enc_nodes = []
    enc_inits = []
    # ReduceMean over spatial blocks: reshape [1,3,S,S] -> [1,3,h,16,w,16]
    # then reduce axes 3 and 5. This is a faithful "pooling encoder".
    enc_nodes.append(helper.make_node(
        "Reshape", ["image", "shape_block"], ["blocks"], name="reshape_blocks"))
    enc_inits.append(numpy_helper.from_array(
        _np().array([1, 3, embed_side, 16, embed_side, 16], dtype="int64"), "shape_block"))
    enc_nodes.append(helper.make_node(
        "ReduceMean", ["blocks"], ["pooled"], axes=[3, 5], keepdims=1, name="pool"))
    enc_nodes.append(helper.make_node(
        "Reshape", ["pooled", "shape_emb"], ["pooled_emb"], name="reshape_emb"))
    enc_inits.append(numpy_helper.from_array(
        _np().array([1, 3, embed_side, embed_side], dtype="int64"), "shape_emb"))
    # Pad channels 3 -> 256 (real MobileSAM is 256 wide). Values are irrelevant;
    # the shape is the contract, and zeros keep the graph deterministic.
    enc_inits.append(numpy_helper.from_array(
        _np().zeros((1, 256 - 3, embed_side, embed_side), dtype="float32"), "chan_pad"))
    enc_nodes.append(helper.make_node(
        "Concat", ["pooled_emb", "chan_pad"], ["embedding"], axis=1, name="widen"))

    enc_graph = helper.make_graph(
        enc_nodes, "test_encoder",
        [helper.make_tensor_value_info("image", TensorProto.FLOAT,
                                       [1, 3, model_size, model_size])],
        [helper.make_tensor_value_info("embedding", TensorProto.FLOAT,
                                       [1, 256, embed_side, embed_side])],
        enc_inits)
    enc_model = helper.make_model(
        enc_graph, opset_imports=[helper.make_opsetid("", 17)],
        producer_name="shadowvision-test-graphs")
    enc_model.ir_version = 9
    enc_path = os.path.join(out_dir, "test_encoder.onnx")
    onnx.save(enc_model, enc_path)

    # ---------------- decoder -------------------------------------------------
    # points [1,N,2] (model px) + labels [1,N] -> logits [1,1,mask_side,mask_side]
    # Oracle: distance from every mask pixel (centre-sampled) to the first
    # foreground point, thresholded at `radius`. Implemented with primitive
    # ops only so every runtime can execute it.
    dec_nodes = []
    dec_inits = []
    # first foreground point: points[0,0,:] (N is fixed at load time by C++)
    dec_nodes.append(helper.make_node(
        "Slice", ["points", "slice_start", "slice_end", "slice_axes"],
        ["pt"], name="take_first_point"))
    dec_inits.append(numpy_helper.from_array(_np().array([0], dtype="int64"), "slice_start"))
    dec_inits.append(numpy_helper.from_array(_np().array([1], dtype="int64"), "slice_end"))
    dec_inits.append(numpy_helper.from_array(_np().array([1], dtype="int64"), "slice_axes"))

    # coordinate grid in *model* px, sampled at mask-cell centres
    grid = _np().stack(_np().meshgrid(
        (_np().arange(mask_side, dtype="float32") + 0.5) * (model_size / mask_side),
        (_np().arange(mask_side, dtype="float32") + 0.5) * (model_size / mask_side),
    ), axis=-1)[None, ...]  # [1,H,W,2]
    dec_inits.append(numpy_helper.from_array(grid.astype("float32"), "grid"))
    dec_nodes.append(helper.make_node("Sub", ["grid", "pt"], ["delta"], name="delta"))
    dec_nodes.append(helper.make_node("Mul", ["delta", "delta"], ["delta2"], name="sq"))
    # opset 17: ReduceSum takes axes as an *input*, not an attribute.
    dec_inits.append(numpy_helper.from_array(_np().array([3], dtype="int64"), "sum_axes"))
    dec_nodes.append(helper.make_node(
        "ReduceSum", ["delta2", "sum_axes"], ["dist2"], keepdims=0, name="dist2"))
    dec_inits.append(numpy_helper.from_array(
        _np().array(radius * radius, dtype="float32"), "r2"))
    dec_nodes.append(helper.make_node("Sub", ["r2", "dist2"], ["margin"], name="margin"))
    # logits: positive inside the disc, negative outside -> the C++ thresholds at 0
    dec_nodes.append(helper.make_node(
        "Unsqueeze", ["margin", "unsq_axes"], ["margin_ch"], name="add_channel"))
    dec_inits.append(numpy_helper.from_array(_np().array([1], dtype="int64"), "unsq_axes"))

    # A single stray positive pixel far from the prompt. Real SAM does this all
    # the time, and it is why ISegmenterDecoder has `keep_largest`. Baking it into
    # the test graph means the largest-component filter is exercised by the
    # integration test instead of being taken on faith.
    speck = _np().full((1, 1, mask_side, mask_side), -5.0, dtype="float32")
    speck[0, 0, mask_side // 8, mask_side * 5 // 8] = 5.0
    dec_inits.append(numpy_helper.from_array(speck, "speck"))
    dec_nodes.append(helper.make_node("Max", ["margin_ch", "speck"], ["logits"], name="add_speck"))

    dec_graph = helper.make_graph(
        dec_nodes, "test_decoder",
        [
            helper.make_tensor_value_info("embedding", TensorProto.FLOAT,
                                          [1, 256, embed_side, embed_side]),
            helper.make_tensor_value_info("points", TensorProto.FLOAT, [1, n_points, 2]),
            helper.make_tensor_value_info("labels", TensorProto.FLOAT, [1, n_points]),
        ],
        [helper.make_tensor_value_info("logits", TensorProto.FLOAT,
                                       [1, 1, mask_side, mask_side])],
        dec_inits)
    dec_model = helper.make_model(
        dec_graph, opset_imports=[helper.make_opsetid("", 17)],
        producer_name="shadowvision-test-graphs")
    dec_model.ir_version = 9
    dec_path = os.path.join(out_dir, "test_decoder.onnx")
    onnx.save(dec_model, dec_path)
    return enc_path, dec_path


def _np():
    import numpy
    return numpy


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="build/testgraphs")
    ap.add_argument("--embed-side", type=int, default=64,
                    help="embedding side; model side = 16x, mask side = 4x")
    ap.add_argument("--radius", type=float, default=40.0, help="oracle disc radius in model px")
    args = ap.parse_args()
    enc, dec = build(args.out, args.embed_side, args.radius)
    # Fail loudly here rather than in C++: a graph that does not load is a bug
    # in this script, not in the segmenter.
    import onnx
    onnx.checker.check_model(enc)
    onnx.checker.check_model(dec)
    print(f"wrote {enc}\nwrote {dec}")
    print(f"model side {args.embed_side * 16}, mask side {args.embed_side * 4}, "
          f"oracle radius {args.radius}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
