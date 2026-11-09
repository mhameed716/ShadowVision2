// SPDX-License-Identifier: MIT
// Backend abstraction: the ONLY place that knows about ONNX Runtime / QNN.
//
// Everything above this line (pipeline, blending, budget, UI) must be testable
// with a synthetic backend and no model files at all — that is how G0 runs on a
// laptop before a single weight has been downloaded.
#pragma once

#include <cstdint>
#include <string>

#include "sv/common.h"

namespace sv {

struct BackendOptions {
    std::string model_path;    // .onnx (dilated LaMa) or "" for synthetic
    int intra_op_threads = 2;  // NO parallelism by default: heat > throughput
    bool use_npu = false;      // QNN HTP when the capability probe allows it
    bool fp16 = true;
    // QNN only: pre-compiled context binary for this exact SoC, if present.
    std::string qnn_context_bin;
    // Refuse to place a graph on the NPU if it would split below this ratio
    // (a split graph round-trips every boundary through the CPU and is slower
    // than CPU-only). 0.9 = "90% of nodes must land on the NPU or don't bother".
    float min_npu_partition_ratio = 0.9f;
};

// Inpainting backend contract. 512x512 RGB in, 512x512 RGB out.
// Mask: 255 = hole. Both tensors are NCHW-normalised internally by the backend.
class IInpaintBackend {
public:
    virtual ~IInpaintBackend() = default;

    virtual Status load(const BackendOptions& opts) = 0;
    virtual Status run(const ImageRGB& image, const Mask& mask, MutableImageRGB out) = 0;
    virtual const char* name() const = 0;
    virtual bool ready() const = 0;

    // Fraction of graph nodes actually claimed by the NPU, -1 when unknown.
    // Reported in the dev console; refuses to be a marketing number.
    virtual float npu_partition_ratio() const { return -1.f; }

    // Frees weights + sessions. Called between stages by the pipeline; the
    // difference between "loaded" and "resident" is the whole memory budget.
    virtual void release() = 0;

    // Opaque per-run diagnostics (e.g. QNN optrace summary path).
    virtual std::string diagnostics() const { return {}; }
};

}  // namespace sv
