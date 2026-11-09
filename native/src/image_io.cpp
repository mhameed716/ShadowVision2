// SPDX-License-Identifier: MIT
#include "sv/image_io.h"

#include <cstdio>
#include <cstring>

namespace sv {

namespace {
// Reads a P5/P6 header, skipping comments. Returns tokens count.
bool ReadHeader(FILE* f, char magic[3], int* w, int* h, int* maxval) {
    if (fscanf(f, "%2s", magic) != 1) return false;
    int c;
    // Skip whitespace/comments
    for (;;) {
        c = fgetc(f);
        if (c == '#') {
            while (c != '\n' && c != EOF) c = fgetc(f);
        } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            continue;
        } else {
            ungetc(c, f);
            break;
        }
    }
    if (fscanf(f, "%d %d %d", w, h, maxval) != 3) return false;
    fgetc(f);  // single whitespace after maxval
    return true;
}
}  // namespace

bool LoadPPM(const std::string& path, OwnedImage* out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char magic[3] = {0};
    int w = 0, h = 0, maxval = 0;
    if (!ReadHeader(f, magic, &w, &h, &maxval) || strcmp(magic, "P6") != 0 || maxval != 255) {
        fclose(f);
        return false;
    }
    out->width = w;
    out->height = h;
    out->data.resize(static_cast<size_t>(w) * h * 3);
    const size_t got = fread(out->data.data(), 1, out->data.size(), f);
    fclose(f);
    if (got != out->data.size()) {
        out->data.clear();
        return false;
    }
    return true;
}

bool SavePPM(const std::string& path, const ImageRGB& img) {
    if (!img.valid()) return false;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n", img.width, img.height);
    for (int y = 0; y < img.height; ++y) {
        fwrite(img.data + static_cast<size_t>(y) * img.stride, 1,
               static_cast<size_t>(img.width) * 3, f);
    }
    fclose(f);
    return true;
}

bool LoadPGM(const std::string& path, OwnedMask* out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char magic[3] = {0};
    int w = 0, h = 0, maxval = 0;
    if (!ReadHeader(f, magic, &w, &h, &maxval) || strcmp(magic, "P5") != 0 || maxval != 255) {
        fclose(f);
        return false;
    }
    out->width = w;
    out->height = h;
    out->data.resize(static_cast<size_t>(w) * h);
    const size_t got = fread(out->data.data(), 1, out->data.size(), f);
    fclose(f);
    if (got != out->data.size()) {
        out->data.clear();
        return false;
    }
    return true;
}

bool SavePGM(const std::string& path, const Mask& mask) {
    if (!mask.valid()) return false;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fprintf(f, "P5\n%d %d\n255\n", mask.width, mask.height);
    for (int y = 0; y < mask.height; ++y) {
        fwrite(mask.data + static_cast<size_t>(y) * mask.stride, 1,
               static_cast<size_t>(mask.width), f);
    }
    fclose(f);
    return true;
}

// Deterministic pseudo-pattern: gradients + hard edges + fine texture.
// Hard edges matter: a flat test image makes every inpainter look perfect and
// hides exactly the seam artefacts we are trying to measure.
void MakeTestPattern(OwnedImage* out, int w, int h, int seed) {
    out->width = w;
    out->height = h;
    out->data.resize(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* p = out->data.data() + (static_cast<size_t>(y) * w + x) * 3;
            const int checker = ((x / 32) + (y / 32) + seed) & 1;
            const int stripe = ((x + y * 3 + seed * 7) % 97) < 12 ? 40 : 0;
            p[0] = static_cast<uint8_t>((x * 255) / (w > 1 ? w - 1 : 1));
            p[1] = static_cast<uint8_t>((y * 255) / (h > 1 ? h - 1 : 1));
            p[2] = static_cast<uint8_t>(checker ? 200 - stripe : 60 + stripe);
        }
    }
}

void MakeRectMask(OwnedMask* out, int w, int h, int x0, int y0, int x1, int y1) {
    out->width = w;
    out->height = h;
    out->data.assign(static_cast<size_t>(w) * h, 0);
    for (int y = y0; y < y1 && y < h; ++y) {
        for (int x = x0; x < x1 && x < w; ++x) {
            out->data[static_cast<size_t>(y) * w + x] = 255;
        }
    }
}

}  // namespace sv
