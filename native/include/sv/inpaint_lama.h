// SPDX-License-Identifier: MIT
// L1 inpainting: LaMa (dilated) through the backend abstraction.
#pragma once

#include <memory>

#include "sv/backend.h"
#include "sv/common.h"

namespace sv {

class LaMaInpainter {
public:
    // `backend` is injected so that the same code runs against ORT on device
    // and against the synthetic/mock backend in tests and on CI.
    explicit LaMaInpainter(std::unique_ptr<IInpaintBackend> backend);
    ~LaMaInpainter();

    Status load(const BackendOptions& opts);
    void release();

    // End-to-end: full-resolution image + mask -> inpainted full-resolution image.
    // Internally: crop the mask bbox with margin, resize to a multiple of 8
    // (LaMa is fully convolutional; 512x512 is the reference, 256 is the floor),
    // run, resize back, feather, colour-correct, blend.
    //
    // `out` must be the same size as `image`. `work_resolution` is the long side
    // of the crop fed to the network (default 512).
    Status inpaint(const ImageRGB& image, const Mask& mask, MutableImageRGB out,
                   const CancelToken& cancel, Stats& stats, int work_resolution = 512);

    // Fixed-size path used by G0 micro-benchmarks (no cropping, no blending).
    Status inpaintFixed(const ImageRGB& image, const Mask& mask, MutableImageRGB out,
                        const CancelToken& cancel, Stats& stats);

    float npu_partition_ratio() const;

    // Fixed-shape models (the ONNX export is 512x512, and QNN rejects dynamic
    // shapes) need a square input. Default false so the synthetic backend still
    // exercises the aspect-preserving path.
    void set_square_input(bool square);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Bounding box of non-zero mask pixels. Returns false for an empty mask.
bool MaskBoundingBox(const Mask& mask, int* x0, int* y0, int* x1, int* y1);

// Expands a bbox by `margin` and clamps it into the image.
void ExpandBox(int* x0, int* y0, int* x1, int* y1, int margin, int w, int h);

}  // namespace sv
