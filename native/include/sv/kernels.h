// SPDX-License-Identifier: MIT
// Deterministic geometry kernels used by the L1 inpainting path.
//
// Deliberately hand-written: these are the operators that (a) run on every
// device regardless of NPU vendor, (b) dominate the cost of the *preview*
// layer L0, and (c) must be bit-reproducible so that G0 can assert on them.
#pragma once

#include <cstdint>

#include "sv/common.h"

namespace sv {

// --- Resize ---------------------------------------------------------------
// Area-average downsample (integer factor f) and bilinear resize.
// Area averaging is used for downsampling because it is the only filter that
// does not alias high-frequency texture into the mask — aliasing here shows up
// later as a shimmering seam.
void DownsampleAreaRGB(const ImageRGB& src, int factor, MutableImageRGB dst);
void DownsampleAreaMask(const Mask& src, int factor, uint8_t* dst, int dst_stride);

void ResizeBilinearRGB(const ImageRGB& src, int dst_w, int dst_h, MutableImageRGB dst);

// Area-average resize to an arbitrary target size (box filter). This is the
// correct filter for *downscaling*: it is what PIL's antialiased resize
// approximates, and the difference is visible on textures (fabric, foliage)
// where point sampling produces moire that the encoder then reads as structure.
void ResizeAreaRGB(const ImageRGB& src, int dst_w, int dst_h, MutableImageRGB dst);
void ResizeBilinearMask(const Mask& src, int dst_w, int dst_h, uint8_t* dst, int dst_stride);

// --- Mask morphology ------------------------------------------------------
// Dilate by `radius` px: the seam is decided by these two calls (+ feather),
// more than by the network. A mask that ends exactly on the object edge always
// leaves a halo of the removed object behind.
void DilateMask(uint8_t* mask, int w, int h, int stride, int radius);
void FeatherMask(uint8_t* mask, int w, int h, int stride, int radius);  // box blur

// --- Blending -------------------------------------------------------------
// Multi-scale blend of `patch` into `base` using `mask` (0..255).
// Laplacian-pyramid style with `levels` bands would be ideal; this is the
// 1-level + 1 low-frequency band version that is cheap enough for a phone and
// removes the colour step at the boundary, which is the visible artefact.
void BlendMaskedRGB(MutableImageRGB base, const ImageRGB& patch, const Mask& mask,
                    uint8_t* scratch, size_t scratch_bytes);

}  // namespace sv
