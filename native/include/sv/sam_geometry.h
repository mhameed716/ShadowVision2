// SPDX-License-Identifier: MIT
// SAM preprocessing geometry — pure functions, no ONNX, no allocation.
//
// This is the part of a segmentation integration that is always wrong the first
// time and is invisible when it is: SAM resizes the long side to 1024, rounds,
// and pads *to the right and bottom* with zeros. Prompt coordinates live in the
// padded 1024 space, so a tap that is not transformed through exactly these
// numbers lands on the wrong object, and the mask comes back plausible but wrong.
//
// Keeping it pure means it is unit-tested on CI with no model and no runtime.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace sv {
namespace sam {

struct Geometry {
    int model_size = 1024;  // square side fed to the encoder
    int new_w = 0;          // resized image width  (<= model_size)
    int new_h = 0;          // resized image height (<= model_size)
    float scale_x = 1.f;    // image px -> model px
    float scale_y = 1.f;

    int pad_right() const { return model_size - new_w; }
    int pad_bottom() const { return model_size - new_h; }
};

// Mirrors SamPredictor.get_preprocess_shape + apply_image: scale the long side to
// `model_size`, round to nearest, never round to a multiple of 16 (the encoder is
// convolutional; the embedding is simply 1024/16 = 64 per side).
inline Geometry PreprocessGeometry(int img_w, int img_h, int model_size = 1024) {
    Geometry g;
    g.model_size = model_size;
    if (img_w <= 0 || img_h <= 0) return g;
    const int long_side = std::max(img_w, img_h);
    const float scale = static_cast<float>(model_size) / static_cast<float>(long_side);
    g.new_h = std::max(1, static_cast<int>(std::lround(img_h * scale)));
    g.new_w = std::max(1, static_cast<int>(std::lround(img_w * scale)));
    // Guard against the long side landing on 1024 - 1 because of rounding.
    if (std::max(g.new_w, g.new_h) > model_size) {
        const float fix = static_cast<float>(model_size) /
                          static_cast<float>(std::max(g.new_w, g.new_h));
        g.new_w = std::max(1, static_cast<int>(std::lround(g.new_w * fix)));
        g.new_h = std::max(1, static_cast<int>(std::lround(g.new_h * fix)));
    }
    g.scale_x = static_cast<float>(g.new_w) / static_cast<float>(img_w);
    g.scale_y = static_cast<float>(g.new_h) / static_cast<float>(img_h);
    return g;
}

// Image pixel -> model coordinate. Padding is at the right/bottom, so the origin
// needs no offset; the coordinate is simply scaled.
inline void TransformPoint(const Geometry& g, float x, float y, float* mx, float* my) {
    *mx = x * g.scale_x;
    *my = y * g.scale_y;
}

// SAM normalisation (RGB, 0..255 input).
//   x' = (x - mean_c) / std_c
// These are the values shipped in SamPredictor; using the ImageNet ones instead is
// a silent 5-10% quality loss that nobody notices until masks start leaking.
constexpr float kMean[3] = {123.675f, 116.28f, 103.53f};
constexpr float kStd[3] = {58.395f, 57.12f, 57.375f};

inline float Normalize(int channel, float value) {
    return (value - kMean[channel]) / kStd[channel];
}

// Model-space mask coordinate -> image pixel (used after upsampling the logits).
inline float InverseMapX(const Geometry& g, float mx) {
    return g.scale_x > 0.f ? mx / g.scale_x : mx;
}
inline float InverseMapY(const Geometry& g, float my) {
    return g.scale_y > 0.f ? my / g.scale_y : my;
}

}  // namespace sam
}  // namespace sv
