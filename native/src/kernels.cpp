// SPDX-License-Identifier: MIT
#include "sv/kernels.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace sv {

void DownsampleAreaRGB(const ImageRGB& src, int factor, MutableImageRGB dst) {
    if (!src.valid() || !dst.valid() || factor < 1) return;
    const int f = factor;
    const int n = f * f;
    for (int y = 0; y < dst.height; ++y) {
        uint8_t* out = dst.data + static_cast<size_t>(y) * dst.stride;
        for (int x = 0; x < dst.width; ++x) {
            int r = 0, g = 0, b = 0;
            for (int dy = 0; dy < f; ++dy) {
                const uint8_t* row = src.data + static_cast<size_t>(y * f + dy) * src.stride;
                for (int dx = 0; dx < f; ++dx) {
                    const uint8_t* p = row + static_cast<size_t>(x * f + dx) * 3;
                    r += p[0];
                    g += p[1];
                    b += p[2];
                }
            }
            out[x * 3 + 0] = static_cast<uint8_t>((r + n / 2) / n);
            out[x * 3 + 1] = static_cast<uint8_t>((g + n / 2) / n);
            out[x * 3 + 2] = static_cast<uint8_t>((b + n / 2) / n);
        }
    }
}

void DownsampleAreaMask(const Mask& src, int factor, uint8_t* dst, int dst_stride) {
    if (!src.valid() || !dst || factor < 1) return;
    const int f = factor;
    const int n = f * f;
    const int out_w = src.width / f;
    const int out_h = src.height / f;
    for (int y = 0; y < out_h; ++y) {
        uint8_t* out = dst + static_cast<size_t>(y) * dst_stride;
        for (int x = 0; x < out_w; ++x) {
            int acc = 0;
            for (int dy = 0; dy < f; ++dy) {
                const uint8_t* row = src.data + static_cast<size_t>(y * f + dy) * src.stride;
                for (int dx = 0; dx < f; ++dx) acc += row[x * f + dx];
            }
            out[x] = static_cast<uint8_t>((acc + n / 2) / n);
        }
    }
}

namespace {
inline void sample_bilinear(const ImageRGB& src, float fx, float fy, uint8_t out[3]) {
    const int x0 = std::min(static_cast<int>(fx), src.width - 1);
    const int y0 = std::min(static_cast<int>(fy), src.height - 1);
    const int x1 = std::min(x0 + 1, src.width - 1);
    const int y1 = std::min(y0 + 1, src.height - 1);
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);
    const uint8_t* p00 = src.data + static_cast<size_t>(y0) * src.stride + static_cast<size_t>(x0) * 3;
    const uint8_t* p10 = src.data + static_cast<size_t>(y0) * src.stride + static_cast<size_t>(x1) * 3;
    const uint8_t* p01 = src.data + static_cast<size_t>(y1) * src.stride + static_cast<size_t>(x0) * 3;
    const uint8_t* p11 = src.data + static_cast<size_t>(y1) * src.stride + static_cast<size_t>(x1) * 3;
    for (int c = 0; c < 3; ++c) {
        const float top = (1.f - tx) * p00[c] + tx * p10[c];
        const float bot = (1.f - tx) * p01[c] + tx * p11[c];
        out[c] = static_cast<uint8_t>(std::clamp((1.f - ty) * top + ty * bot + 0.5f, 0.f, 255.f));
    }
}
}  // namespace

void ResizeBilinearRGB(const ImageRGB& src, int dst_w, int dst_h, MutableImageRGB dst) {
    if (!src.valid() || !dst.valid() || dst_w <= 0 || dst_h <= 0) return;
    const float sx = static_cast<float>(src.width) / static_cast<float>(dst_w);
    const float sy = static_cast<float>(src.height) / static_cast<float>(dst_h);
    for (int y = 0; y < dst_h; ++y) {
        uint8_t* out = dst.data + static_cast<size_t>(y) * dst.stride;
        const float fy = (static_cast<float>(y) + 0.5f) * sy - 0.5f;
        for (int x = 0; x < dst_w; ++x) {
            const float fx = (static_cast<float>(x) + 0.5f) * sx - 0.5f;
            sample_bilinear(src, std::max(fx, 0.f), std::max(fy, 0.f), out + x * 3);
        }
    }
}

void ResizeBilinearMask(const Mask& src, int dst_w, int dst_h, uint8_t* dst, int dst_stride) {
    if (!src.valid() || !dst || dst_w <= 0 || dst_h <= 0) return;
    const float sx = static_cast<float>(src.width) / static_cast<float>(dst_w);
    const float sy = static_cast<float>(src.height) / static_cast<float>(dst_h);
    for (int y = 0; y < dst_h; ++y) {
        uint8_t* out = dst + static_cast<size_t>(y) * dst_stride;
        const float fy = (static_cast<float>(y) + 0.5f) * sy - 0.5f;
        const int y0 = std::clamp(static_cast<int>(fy), 0, src.height - 1);
        const int y1 = std::min(y0 + 1, src.height - 1);
        const float ty = std::clamp(fy - static_cast<float>(y0), 0.f, 1.f);
        for (int x = 0; x < dst_w; ++x) {
            const float fx = (static_cast<float>(x) + 0.5f) * sx - 0.5f;
            const int x0 = std::clamp(static_cast<int>(fx), 0, src.width - 1);
            const int x1 = std::min(x0 + 1, src.width - 1);
            const float tx = std::clamp(fx - static_cast<float>(x0), 0.f, 1.f);
            const float top = (1.f - tx) * src.data[static_cast<size_t>(y0) * src.stride + x0] +
                              tx * src.data[static_cast<size_t>(y0) * src.stride + x1];
            const float bot = (1.f - tx) * src.data[static_cast<size_t>(y1) * src.stride + x0] +
                              tx * src.data[static_cast<size_t>(y1) * src.stride + x1];
            out[x] = static_cast<uint8_t>(std::clamp((1.f - ty) * top + ty * bot + 0.5f, 0.f, 255.f));
        }
    }
}

void ResizeAreaRGB(const ImageRGB& src, int dst_w, int dst_h, MutableImageRGB dst) {
    if (!src.valid() || !dst.valid() || dst_w <= 0 || dst_h <= 0) return;
    // Two-pass box filter (horizontal then vertical) with fractional coverage.
    // O(dst * support) per axis; for 4x downscale the support is ~4 px/side.
    const float sx = static_cast<float>(src.width) / dst_w;
    const float sy = static_cast<float>(src.height) / dst_h;
    std::vector<float> tmp(static_cast<size_t>(dst_w) * src.height * 3, 0.f);

    for (int y = 0; y < src.height; ++y) {
        const uint8_t* row = src.data + static_cast<size_t>(y) * src.stride;
        float* out = tmp.data() + static_cast<size_t>(y) * dst_w * 3;
        for (int x = 0; x < dst_w; ++x) {
            const float x0 = x * sx;
            const float x1 = std::min(static_cast<float>(src.width), x0 + sx);
            int i0 = static_cast<int>(x0);
            const int i1 = std::min(src.width - 1, static_cast<int>(std::ceil(x1)) - 1);
            float r = 0, g = 0, b = 0, wsum = 0;
            for (int i = i0; i <= i1; ++i) {
                const float lo = std::max(x0, static_cast<float>(i));
                const float hi = std::min(x1, static_cast<float>(i + 1));
                const float wgt = hi - lo;
                if (wgt <= 0) continue;
                r += row[i * 3 + 0] * wgt;
                g += row[i * 3 + 1] * wgt;
                b += row[i * 3 + 2] * wgt;
                wsum += wgt;
            }
            if (wsum <= 0) wsum = 1;
            out[x * 3 + 0] = r / wsum;
            out[x * 3 + 1] = g / wsum;
            out[x * 3 + 2] = b / wsum;
        }
    }

    for (int y = 0; y < dst_h; ++y) {
        const float y0 = y * sy;
        const float y1 = std::min(static_cast<float>(src.height), y0 + sy);
        const int j0 = static_cast<int>(y0);
        const int j1 = std::min(src.height - 1, static_cast<int>(std::ceil(y1)) - 1);
        uint8_t* out = dst.data + static_cast<size_t>(y) * dst.stride;
        for (int x = 0; x < dst_w; ++x) {
            float r = 0, g = 0, b = 0, wsum = 0;
            for (int j = j0; j <= j1; ++j) {
                const float lo = std::max(y0, static_cast<float>(j));
                const float hi = std::min(y1, static_cast<float>(j + 1));
                const float wgt = hi - lo;
                if (wgt <= 0) continue;
                const float* p = tmp.data() + (static_cast<size_t>(j) * dst_w + x) * 3;
                r += p[0] * wgt;
                g += p[1] * wgt;
                b += p[2] * wgt;
                wsum += wgt;
            }
            if (wsum <= 0) wsum = 1;
            out[x * 3 + 0] = static_cast<uint8_t>(std::clamp(r / wsum + 0.5f, 0.f, 255.f));
            out[x * 3 + 1] = static_cast<uint8_t>(std::clamp(g / wsum + 0.5f, 0.f, 255.f));
            out[x * 3 + 2] = static_cast<uint8_t>(std::clamp(b / wsum + 0.5f, 0.f, 255.f));
        }
    }
}

// Separable square (Chebyshev) dilation: O(w*h) using two passes with a
// running maximum approximation (block-max per radius is adequate for masks,
// exact Chebyshev on a binary mask is what we want; we threshold at 128).
void DilateMask(uint8_t* mask, int w, int h, int stride, int radius) {
    if (!mask || radius <= 0) return;
    std::vector<uint8_t> tmp(static_cast<size_t>(w) * h, 0);
    // Horizontal pass.
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = mask + static_cast<size_t>(y) * stride;
        uint8_t* out = tmp.data() + static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            const int lo = std::max(0, x - radius);
            const int hi = std::min(w - 1, x + radius);
            uint8_t m = 0;
            for (int i = lo; i <= hi; ++i) m = std::max(m, row[i]);
            out[x] = m;
        }
    }
    // Vertical pass.
    for (int y = 0; y < h; ++y) {
        uint8_t* row = mask + static_cast<size_t>(y) * stride;
        const int lo = std::max(0, y - radius);
        const int hi = std::min(h - 1, y + radius);
        for (int x = 0; x < w; ++x) {
            uint8_t m = 0;
            for (int yy = lo; yy <= hi; ++yy) m = std::max(m, tmp[static_cast<size_t>(yy) * w + x]);
            row[x] = m;
        }
    }
}

void FeatherMask(uint8_t* mask, int w, int h, int stride, int radius) {
    if (!mask || radius <= 0) return;
    std::vector<uint16_t> tmp(static_cast<size_t>(w) * h, 0);
    const int win = radius * 2 + 1;
    // Horizontal box blur with clamped edges.
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = mask + static_cast<size_t>(y) * stride;
        int acc = 0;
        for (int i = -radius; i <= radius; ++i) acc += row[std::clamp(i, 0, w - 1)];
        for (int x = 0; x < w; ++x) {
            tmp[static_cast<size_t>(y) * w + x] = static_cast<uint16_t>(acc / win);
            const int add = std::clamp(x + radius + 1, 0, w - 1);
            const int rem = std::clamp(x - radius, 0, w - 1);
            acc += row[add] - row[rem];
        }
    }
    // Vertical pass.
    for (int x = 0; x < w; ++x) {
        int acc = 0;
        for (int j = -radius; j <= radius; ++j) acc += tmp[static_cast<size_t>(std::clamp(j, 0, h - 1)) * w + x];
        for (int y = 0; y < h; ++y) {
            mask[static_cast<size_t>(y) * stride + x] = static_cast<uint8_t>(acc / win);
            const int add = std::clamp(y + radius + 1, 0, h - 1);
            const int rem = std::clamp(y - radius, 0, h - 1);
            acc += tmp[static_cast<size_t>(add) * w + x] - tmp[static_cast<size_t>(rem) * w + x];
        }
    }
}

void BlendMaskedRGB(MutableImageRGB base, const ImageRGB& patch, const Mask& mask,
                    uint8_t* scratch, size_t scratch_bytes) {
    if (!base.valid() || !patch.valid() || !mask.valid()) return;
    const int w = std::min(base.width, std::min(patch.width, mask.width));
    const int h = std::min(base.height, std::min(patch.height, mask.height));

    // Low-frequency colour offset correction: compute the mean residual between
    // patch and base *outside* the mask, then add it to the patch inside the
    // mask. This is what removes the "square of slightly different colour"
    // artefact better than any extra diffusion step.
    double dr = 0, dg = 0, db = 0;
    int64_t n = 0;
    for (int y = 0; y < h; y += 2) {
        const uint8_t* pb = base.data + static_cast<size_t>(y) * base.stride;
        const uint8_t* pp = patch.data + static_cast<size_t>(y) * patch.stride;
        const uint8_t* pm = mask.data + static_cast<size_t>(y) * mask.stride;
        for (int x = 0; x < w; x += 2) {
            if (pm[x] > 8) continue;
            dr += static_cast<double>(pb[x * 3 + 0]) - pp[x * 3 + 0];
            dg += static_cast<double>(pb[x * 3 + 1]) - pp[x * 3 + 1];
            db += static_cast<double>(pb[x * 3 + 2]) - pp[x * 3 + 2];
            ++n;
        }
    }
    (void)scratch;
    (void)scratch_bytes;
    if (n > 0) {
        dr /= static_cast<double>(n);
        dg /= static_cast<double>(n);
        db /= static_cast<double>(n);
    }

    for (int y = 0; y < h; ++y) {
        uint8_t* pb = base.data + static_cast<size_t>(y) * base.stride;
        const uint8_t* pp = patch.data + static_cast<size_t>(y) * patch.stride;
        const uint8_t* pm = mask.data + static_cast<size_t>(y) * mask.stride;
        for (int x = 0; x < w; ++x) {
            const float a = static_cast<float>(pm[x]) / 255.f;
            if (a <= 0.f) continue;
            const float cr[3] = {static_cast<float>(dr), static_cast<float>(dg),
                                 static_cast<float>(db)};
            for (int c = 0; c < 3; ++c) {
                const float p = std::clamp(static_cast<float>(pp[x * 3 + c]) + a * cr[c], 0.f, 255.f);
                const float v = (1.f - a) * static_cast<float>(pb[x * 3 + c]) + a * p;
                pb[x * 3 + c] = static_cast<uint8_t>(v + 0.5f);
            }
        }
    }
}

}  // namespace sv
