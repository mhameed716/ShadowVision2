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


def build_acly(out_dir: str, model_side: int = 256, radius: float = 40.0) -> tuple[str, str]:
    """Same oracle behaviour, but with the *Acly/MobileSAM interface*.

    Encoder input is NHWC [S,S,3] (it pads/normalises internally in the real
    model), decoder takes point_coords/point_labels/mask_input/has_mask_input/
    orig_im_size and returns masks already at the original image size. Supporting
    this layout matters because it is the MobileSAM ONNX most people can actually
    download; if only our own export worked, "MobileSAM support" would be true
    only on the machine that wrote the export script.
    """
    import onnx
    from onnx import TensorProto, helper, numpy_helper
    np = _np()
    os.makedirs(out_dir, exist_ok=True)
    embed_side = model_side // 16
    mask_side = model_side // 4
    n_points = 4

    # --- encoder (NHWC) ---------------------------------------------------
    enc_nodes, enc_inits = [], []
    enc_nodes.append(helper.make_node("Reshape", ["image", "shape_block"], ["blocks"]))
    enc_inits.append(numpy_helper.from_array(
        np.array([1, embed_side, 16, embed_side, 16, 3], dtype="int64"), "shape_block"))
    # ReduceMean over axes 2 and 4 (H,W blocks); keep channel axis separate by
    # transposing the channel axis forward first.
    enc_nodes.append(helper.make_node("Transpose", ["blocks"], ["bt"], perm=[0, 5, 1, 2, 3, 4]))
    enc_nodes.append(helper.make_node("ReduceMean", ["bt"], ["pooled"],
                                      axes=[3, 5], keepdims=1))
    enc_nodes.append(helper.make_node("Reshape", ["pooled", "shape_emb"], ["pooled_emb"]))
    enc_inits.append(numpy_helper.from_array(
        np.array([1, 3, embed_side, embed_side], dtype="int64"), "shape_emb"))
    enc_inits.append(numpy_helper.from_array(
        np.zeros((1, 256 - 3, embed_side, embed_side), dtype="float32"), "chan_pad"))
    enc_nodes.append(helper.make_node("Concat", ["pooled_emb", "chan_pad"], ["embedding"],
                                      axis=1))
    enc_graph = helper.make_graph(
        enc_nodes, "acly_encoder",
        [helper.make_tensor_value_info("image", TensorProto.FLOAT,
                                       [model_side, model_side, 3])],
        [helper.make_tensor_value_info("embedding", TensorProto.FLOAT,
                                       [1, 256, embed_side, embed_side])],
        enc_inits)
    enc_model = helper.make_model(enc_graph, opset_imports=[helper.make_opsetid("", 17)],
                                 producer_name="shadowvision-test-graphs")
    enc_model.ir_version = 9
    enc_path = os.path.join(out_dir, "acly_encoder.onnx")
    onnx.save(enc_model, enc_path)

    # --- decoder ----------------------------------------------------------
    dec_nodes, dec_inits = [], []
    # First point: Gather on the points axis (1) gives [1,2]. Slicing axis 2
    # directly would keep the N axis and silently make every downstream tensor
    # rank-3, which then shows up as "decoder output is rank 5".
    dec_inits.append(numpy_helper.from_array(np.array(0, dtype="int64"), "idx0"))
    dec_nodes.append(helper.make_node("Gather", ["point_coords", "idx0"], ["pt"],
                                      axis=1, name="first_point"))
    dec_inits.append(numpy_helper.from_array(np.array([0], dtype="int64"), "s"))
    dec_inits.append(numpy_helper.from_array(np.array([1], dtype="int64"), "e"))
    dec_inits.append(numpy_helper.from_array(np.array([1], dtype="int64"), "ax"))
    # Grid in *original image* pixels: orig_im_size is [h,w] (SAM's convention),
    # so the oracle emits directly at that resolution like the real graph does.
    dec_inits.append(numpy_helper.from_array(np.array([0], dtype="int64"), "ax0"))
    # Rather than fight symbolic shapes, expand a meshgrid from orig_im_size with
    # Range ops: x in [0,w), y in [0,h), then distance to the first point.
    dec_nodes.append(helper.make_node("Slice", ["orig_im_size", "i0", "i1", "ax0"],
                                      ["oh"], name="oh"))
    dec_nodes.append(helper.make_node("Slice", ["orig_im_size", "i1", "i2", "ax0"],
                                      ["ow"], name="ow"))
    dec_inits.append(numpy_helper.from_array(np.array([0], dtype="int64"), "i0"))
    dec_inits.append(numpy_helper.from_array(np.array([1], dtype="int64"), "i1"))
    dec_inits.append(numpy_helper.from_array(np.array([2], dtype="int64"), "i2"))
    dec_inits.append(numpy_helper.from_array(np.array(0, dtype="float32"), "r0"))
    dec_inits.append(numpy_helper.from_array(np.array(1, dtype="float32"), "r1"))
    dec_nodes.append(helper.make_node("Range", ["r0", "ow", "r1"], ["xs"], name="xs"))
    dec_nodes.append(helper.make_node("Range", ["r0", "oh", "r1"], ["ys"], name="ys"))
    dec_nodes.append(helper.make_node("Unsqueeze", ["xs", "ax0"], ["xs2"], name="xs2"))
    dec_nodes.append(helper.make_node("Unsqueeze", ["ys", "ax1"], ["ys2"], name="ys2"))
    dec_inits.append(numpy_helper.from_array(np.array([1], dtype="int64"), "ax1"))
    # delta = (x - px)^2 + (y - py)^2 using broadcasting [h,1] and [1,w]
    dec_nodes.append(helper.make_node("Slice", ["pt", "s", "e", "ax2"], ["px"], name="px"))
    dec_nodes.append(helper.make_node("Slice", ["pt", "s2", "e2", "ax2"], ["py"], name="py"))
    dec_inits.append(numpy_helper.from_array(np.array([1], dtype="int64"), "s2"))
    dec_inits.append(numpy_helper.from_array(np.array([2], dtype="int64"), "e2"))
    dec_inits.append(numpy_helper.from_array(np.array([1], dtype="int64"), "ax2"))
    # The real Acly decoder receives point_coords in the RESIZED model frame
    # (long side = model_side), and orig_im_size only tells it what to upsample
    # to. So the oracle must scale the point back up before comparing it with the
    # original-pixel grid: orig_px = point * orig_size / model_side.
    dec_inits.append(numpy_helper.from_array(
        np.array([float(model_side)], dtype="float32"), "model_side_f"))
    dec_nodes.append(helper.make_node("Mul", ["px", "ow"], ["pxs"], name="pxs"))
    dec_nodes.append(helper.make_node("Div", ["pxs", "model_side_f"], ["px_orig"], name="pxo"))
    dec_nodes.append(helper.make_node("Mul", ["py", "oh"], ["pys"], name="pys"))
    dec_nodes.append(helper.make_node("Div", ["pys", "model_side_f"], ["py_orig"], name="pyo"))
    dec_nodes.append(helper.make_node("Sub", ["xs2", "px_orig"], ["dx"], name="dx"))
    dec_nodes.append(helper.make_node("Sub", ["ys2", "py_orig"], ["dy"], name="dy"))
    dec_nodes.append(helper.make_node("Mul", ["dx", "dx"], ["dx2"], name="dx2"))
    dec_nodes.append(helper.make_node("Mul", ["dy", "dy"], ["dy2"], name="dy2"))
    dec_nodes.append(helper.make_node("Add", ["dx2", "dy2"], ["d2"], name="d2"))
    dec_inits.append(numpy_helper.from_array(np.array(radius * radius, dtype="float32"), "rr"))
    dec_nodes.append(helper.make_node("Sub", ["rr", "d2"], ["margin"], name="margin"))
    # Second, tiny disc anchored near the top-left corner (1/8 of the frame). It
    # plays the role of the specks real SAM returns; keep_largest must remove it
    # and must NOT remove it when the flag is off.
    dec_nodes.append(helper.make_node("Mul", ["ow", "eighth"], ["sx"], name="sx"))
    dec_nodes.append(helper.make_node("Mul", ["oh", "eighth"], ["sy"], name="sy"))
    dec_inits.append(numpy_helper.from_array(np.array([0.125], dtype="float32"), "eighth"))
    dec_nodes.append(helper.make_node("Sub", ["xs2", "sx"], ["sdx"], name="sdx"))
    dec_nodes.append(helper.make_node("Sub", ["ys2", "sy"], ["sdy"], name="sdy"))
    dec_nodes.append(helper.make_node("Mul", ["sdx", "sdx"], ["sdx2"], name="sdx2"))
    dec_nodes.append(helper.make_node("Mul", ["sdy", "sdy"], ["sdy2"], name="sdy2"))
    dec_nodes.append(helper.make_node("Add", ["sdx2", "sdy2"], ["sd2"], name="sd2"))
    dec_inits.append(numpy_helper.from_array(np.array(9.0, dtype="float32"), "sr2"))
    dec_nodes.append(helper.make_node("Sub", ["sr2", "sd2"], ["speck"], name="speck"))
    dec_nodes.append(helper.make_node("Max", ["margin", "speck"], ["margin2"], name="mx"))
    # margin is [h,w]; one Unsqueeze to [1,1,h,w] — the real single-mask decoder
    # output is rank 4, and a rank mismatch here is exactly the kind of thing the
    # loader must reject rather than guess at.
    dec_nodes.append(helper.make_node("Unsqueeze", ["margin2", "ax01"], ["masks"], name="m3"))
    dec_inits.append(numpy_helper.from_array(np.array([0, 1], dtype="int64"), "ax01"))

    dec_graph = helper.make_graph(
        dec_nodes, "acly_decoder",
        [
            helper.make_tensor_value_info("image_embeddings", TensorProto.FLOAT,
                                          [1, 256, embed_side, embed_side]),
            helper.make_tensor_value_info("point_coords", TensorProto.FLOAT, [1, n_points, 2]),
            helper.make_tensor_value_info("point_labels", TensorProto.FLOAT, [1, n_points]),
            helper.make_tensor_value_info("mask_input", TensorProto.FLOAT, [1, 1, 256, 256]),
            helper.make_tensor_value_info("has_mask_input", TensorProto.FLOAT, [1]),
            helper.make_tensor_value_info("orig_im_size", TensorProto.FLOAT, [2]),
        ],
        [helper.make_tensor_value_info("masks", TensorProto.FLOAT, [1, 1, None, None])],
        dec_inits)
    dec_model = helper.make_model(dec_graph, opset_imports=[helper.make_opsetid("", 17)],
                                  producer_name="shadowvision-test-graphs")
    dec_model.ir_version = 9
    dec_path = os.path.join(out_dir, "acly_decoder.onnx")
    onnx.save(dec_model, dec_path)
    return enc_path, dec_path


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
    ap.add_argument("--interface", choices=["ours", "acly"], default="ours",
                    help="which decoder interface to imitate")
    args = ap.parse_args()
    if args.interface == "acly":
        enc, dec = build_acly(args.out, args.embed_side * 16, args.radius)
    else:
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
