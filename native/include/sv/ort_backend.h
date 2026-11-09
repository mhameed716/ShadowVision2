// SPDX-License-Identifier: MIT
// ONNX Runtime backend: the only translation unit that includes <onnxruntime_cxx_api.h>.
//
// Implemented in src/ort_backend.cpp behind -DSV_WITH_ORT=ON so that the G0
// harness always builds and runs on machines/CI that have no ORT and no weights.
//
// Device notes (docs/decisions-ar.md D4):
//   * CPU EP (XNNPACK where available) is the floor and the only path we claim.
//   * QNN EP is opt-in per device, requires a quantized/fp16 static-shape model
//     plus a context binary compiled for that exact SoC (v69/v73/v75/v79/v81).
//     If the NPU would claim less than min_npu_partition_ratio of the nodes we
//     fall back to CPU instead of shipping a graph that ping-pongs across the
//     bus (that is slower than CPU-only, and nobody measures it).
#pragma once

#include <memory>

#include "sv/backend.h"

namespace sv {

std::unique_ptr<IInpaintBackend> CreateOrtBackend();

}  // namespace sv
