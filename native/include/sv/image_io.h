// SPDX-License-Identifier: MIT
// Minimal, dependency-free image IO for the G0 harness.
//
// PPM/PGM only, on purpose:
//   * zero third-party code in the measurement path (no libpng skewing RSS),
//   * readable by every image viewer and by PIL for the reference quality check,
//   * the harness must be able to run on the device itself with no Java.
#pragma once

#include <string>
#include <vector>

#include "sv/common.h"

namespace sv {

struct OwnedImage {
    std::vector<uint8_t> data;
    int width = 0;
    int height = 0;

    ImageRGB view() const { return ImageRGB{data.data(), width, height, width * 3}; }
    MutableImageRGB mutable_view() { return MutableImageRGB{data.data(), width, height, width * 3}; }
    bool valid() const { return !data.empty() && width > 0 && height > 0; }
};

struct OwnedMask {
    std::vector<uint8_t> data;
    int width = 0;
    int height = 0;

    Mask view() const { return Mask{data.data(), width, height, width}; }
    bool valid() const { return !data.empty() && width > 0 && height > 0; }
};

bool LoadPPM(const std::string& path, OwnedImage* out);
bool SavePPM(const std::string& path, const ImageRGB& img);
bool LoadPGM(const std::string& path, OwnedMask* out);
bool SavePGM(const std::string& path, const Mask& mask);

// Test data generators (deterministic, no RNG dependency).
void MakeTestPattern(OwnedImage* out, int w, int h, int seed);
void MakeRectMask(OwnedMask* out, int w, int h, int x0, int y0, int x1, int y1);

}  // namespace sv
