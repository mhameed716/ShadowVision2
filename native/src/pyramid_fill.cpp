// SPDX-License-Identifier: MIT
// Multi-scale hole filling: what the no-model backend uses instead of a network.
//
// The first implementation ran eight ring-fill passes and returned. Eight passes
// advance roughly eight pixels, so any hole wider than that keeps its original
// pixels in the middle — and the backend still reported Status::Ok, so the user
// saw "removed" with the object still there. That is exactly what the end-to-end
// demo caught: the thin cable vanished, the 46-px-wide post did not.
//
// The fix is the standard trick: solve the hole at a coarse scale where it is only
// a few pixels across, then refine downwards. Deterministic, no randomness, and
// O(pixels) per level instead of O(radius) per pixel.
#include "sv/synthetic_backend.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "sv/memory.h"

namespace sv {
namespace {

struct Level {
    std::vector<uint8_t> rgb;    // w*h*3
    std::vector<uint8_t> mask;   // w*h, 0 = known
    int w = 0;
    int h = 0;
};

// One ring-fill pass over every hole pixel that touches a known pixel.
// Returns the number of pixels filled.
int fill_pass_level(Level* lv) {
    static const int dx[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
    static const int dy[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
    const int w = lv->w, h = lv->h;
    std::vector<uint8_t> upd(static_cast<size_t>(w) * h * 4, 0);
    int filled = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t p = static_cast<size_t>(y) * w + x;
            if (lv->mask[p] == 0) continue;
            float sr = 0, sg = 0, sb = 0, sw = 0;
            for (int k = 0; k < 8; ++k) {
                const int nx = x + dx[k], ny = y + dy[k];
                if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                const size_t q = static_cast<size_t>(ny) * w + nx;
                if (lv->mask[q] != 0) continue;
                const float wgt = (k == 0 || k == 2 || k == 5 || k == 7) ? 0.7071f : 1.0f;
                sr += lv->rgb[q * 3 + 0] * wgt;
                sg += lv->rgb[q * 3 + 1] * wgt;
                sb += lv->rgb[q * 3 + 2] * wgt;
                sw += wgt;
            }
            if (sw <= 0.f) continue;
            upd[p * 4 + 0] = static_cast<uint8_t>(sr / sw);
            upd[p * 4 + 1] = static_cast<uint8_t>(sg / sw);
            upd[p * 4 + 2] = static_cast<uint8_t>(sb / sw);
            upd[p * 4 + 3] = 1;
            ++filled;
        }
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t p = static_cast<size_t>(y) * w + x;
            if (!upd[p * 4 + 3]) continue;
            lv->rgb[p * 3 + 0] = upd[p * 4 + 0];
            lv->rgb[p * 3 + 1] = upd[p * 4 + 1];
            lv->rgb[p * 3 + 2] = upd[p * 4 + 2];
            lv->mask[p] = 0;
        }
    }
    return filled;
}

int fill_to_convergence(Level* lv, int max_passes) {
    int total = 0;
    for (int i = 0; i < max_passes; ++i) {
        const int n = fill_pass_level(lv);
        if (n == 0) break;
        total += n;
    }
    return total;
}

int remaining_hole(const Level& lv) {
    int n = 0;
    for (uint8_t m : lv.mask) n += (m != 0);
    return n;
}

// Half-resolution: a coarse pixel is known if ANY child is known, and its colour
// is the mean of the known children. Doing it mask-aware is what stops the hole
// from bleeding its (wrong) colour into the coarse level.
Level downsample(const Level& src) {
    Level dst;
    dst.w = std::max(1, src.w / 2);
    dst.h = std::max(1, src.h / 2);
    dst.rgb.assign(static_cast<size_t>(dst.w) * dst.h * 3, 0);
    dst.mask.assign(static_cast<size_t>(dst.w) * dst.h, 1);
    for (int y = 0; y < dst.h; ++y) {
        for (int x = 0; x < dst.w; ++x) {
            int sr = 0, sg = 0, sb = 0, n = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const int sx = x * 2 + dx, sy = y * 2 + dy;
                    if (sx >= src.w || sy >= src.h) continue;
                    const size_t p = static_cast<size_t>(sy) * src.w + sx;
                    if (src.mask[p] != 0) continue;  // hole: has no colour to give
                    sr += src.rgb[p * 3 + 0];
                    sg += src.rgb[p * 3 + 1];
                    sb += src.rgb[p * 3 + 2];
                    ++n;
                }
            }
            const size_t q = static_cast<size_t>(y) * dst.w + x;
            if (n > 0) {
                dst.rgb[q * 3 + 0] = static_cast<uint8_t>(sr / n);
                dst.rgb[q * 3 + 1] = static_cast<uint8_t>(sg / n);
                dst.rgb[q * 3 + 2] = static_cast<uint8_t>(sb / n);
                dst.mask[q] = 0;
            }
        }
    }
    return dst;
}

// Fill the fine level's holes from the (already complete) coarse level, then let
// the ring fill finish whatever the upsampling could not express.
void upsample_into(Level* fine, const Level& coarse) {
    for (int y = 0; y < fine->h; ++y) {
        for (int x = 0; x < fine->w; ++x) {
            const size_t p = static_cast<size_t>(y) * fine->w + x;
            if (fine->mask[p] == 0) continue;
            // nearest coarse sample
            const int cx = std::min(coarse.w - 1, x / 2);
            const int cy = std::min(coarse.h - 1, y / 2);
            const size_t q = static_cast<size_t>(cy) * coarse.w + cx;
            fine->rgb[p * 3 + 0] = coarse.rgb[q * 3 + 0];
            fine->rgb[p * 3 + 1] = coarse.rgb[q * 3 + 1];
            fine->rgb[p * 3 + 2] = coarse.rgb[q * 3 + 2];
            fine->mask[p] = 0;
        }
    }
}

}  // namespace

// Kept as a primitive: the single-scale ring fill used by the tests and by the
// final refinement at the finest level.
void TeleaFill(uint8_t* rgb, int rgb_stride, uint8_t* mask, int mask_stride, int w, int h,
               int iterations) {
    Level lv;
    lv.w = w;
    lv.h = h;
    lv.rgb.resize(static_cast<size_t>(w) * h * 3);
    lv.mask.resize(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        std::memcpy(lv.rgb.data() + static_cast<size_t>(y) * w * 3,
                    rgb + static_cast<size_t>(y) * rgb_stride, static_cast<size_t>(w) * 3);
        std::memcpy(lv.mask.data() + static_cast<size_t>(y) * w,
                    mask + static_cast<size_t>(y) * mask_stride, static_cast<size_t>(w));
    }
    for (int i = 0; i < iterations; ++i) {
        if (fill_pass_level(&lv) == 0) break;
    }
    for (int y = 0; y < h; ++y) {
        std::memcpy(rgb + static_cast<size_t>(y) * rgb_stride,
                    lv.rgb.data() + static_cast<size_t>(y) * w * 3,
                    static_cast<size_t>(w) * 3);
        std::memcpy(mask + static_cast<size_t>(y) * mask_stride,
                    lv.mask.data() + static_cast<size_t>(y) * w, static_cast<size_t>(w));
    }
}

int PyramidFill(uint8_t* rgb, int rgb_stride, uint8_t* mask, int mask_stride, int w, int h) {
    if (!rgb || !mask || w <= 0 || h <= 0) return 0;

    Level base;
    base.w = w;
    base.h = h;
    base.rgb.resize(static_cast<size_t>(w) * h * 3);
    base.mask.resize(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        std::memcpy(base.rgb.data() + static_cast<size_t>(y) * w * 3,
                    rgb + static_cast<size_t>(y) * rgb_stride, static_cast<size_t>(w) * 3);
        std::memcpy(base.mask.data() + static_cast<size_t>(y) * w,
                    mask + static_cast<size_t>(y) * mask_stride, static_cast<size_t>(w));
    }

    const int before = remaining_hole(base);
    if (before == 0) return 0;

    // Snapshot the hole NOW. After the fill the mask is all zeros, so reading it
    // later yields an empty hole and the smoothing pass below silently does
    // nothing — which is exactly the bug that made the blocks stay on screen.
    std::vector<uint8_t> hole(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < hole.size(); ++i) hole[i] = base.mask[i] != 0 ? 1 : 0;

    // Build the pyramid until the hole is small, then fill bottom-up.
    std::vector<Level> pyr;
    pyr.push_back(std::move(base));
    while (pyr.back().w > 8 && pyr.back().h > 8) {
        Level next = downsample(pyr.back());
        // Stop if the level is entirely hole: nothing to constrain it with.
        bool any_known = false;
        for (uint8_t m : next.mask) {
            if (m == 0) { any_known = true; break; }
        }
        if (!any_known) break;
        pyr.push_back(std::move(next));
    }

    // Coarsest first: converge there (the hole is a few pixels at this scale).
    fill_to_convergence(&pyr.back(), 4096);

    // Refine downwards.
    for (int i = static_cast<int>(pyr.size()) - 2; i >= 0; --i) {
        upsample_into(&pyr[i], pyr[i + 1]);
        // A few passes to re-fill anything the nearest-neighbour upsample could
        // not cover (thin diagonal structures).
        fill_to_convergence(&pyr[i], 64);
    }

    Level& out = pyr.front();

    // The pyramid gets the COLOUR roughly right but leaves visible blocks: it
    // fills coarse pixels with a mean and upsamples with nearest-neighbour, so a
    // hole that spans a colour gradient comes back as a staircase. That is exactly
    // the grey column left where the post used to be in the end-to-end run.
    //
    // Solve Laplace's equation over the hole instead, with the known pixels as
    // fixed boundary conditions: the result is the smoothest surface that matches
    // the surroundings, which is what "the object was never there" looks like.
    // The pyramid's result is only the initial guess now, which is why it
    // converges in a few hundred Jacobi sweeps instead of tens of thousands.
    // The pyramid's result is the initial guess; Poisson relaxation below makes
    // it smooth.
    int relax_iters = 0;
    for (uint8_t v : hole) relax_iters += (v != 0);
    relax_iters = std::min(600, 40 + relax_iters / 8);
    std::vector<float> cur(static_cast<size_t>(w) * h * 3);
    std::vector<float> nxt(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0; i < cur.size(); ++i) cur[i] = out.rgb[i];
    nxt = cur;
    for (int it = 0; it < relax_iters; ++it) {
        int changed = 0;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t p = static_cast<size_t>(y) * w + x;
                if (!hole[p]) continue;
                float sr = 0, sg = 0, sb = 0;
                int n = 0;
                const int nb[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
                for (const auto& q : nb) {
                    if (q[0] < 0 || q[1] < 0 || q[0] >= w || q[1] >= h) continue;
                    const size_t k = static_cast<size_t>(q[1]) * w + q[0];
                    sr += cur[k * 3 + 0];
                    sg += cur[k * 3 + 1];
                    sb += cur[k * 3 + 2];
                    ++n;
                }
                if (n == 0) continue;
                nxt[p * 3 + 0] = sr / n;
                nxt[p * 3 + 1] = sg / n;
                nxt[p * 3 + 2] = sb / n;
                ++changed;
            }
        }
        if (changed == 0) break;
        // Copy only the hole pixels back; known pixels never move.
        for (size_t i = 0; i < hole.size(); ++i) {
            if (!hole[i]) continue;
            cur[i * 3 + 0] = nxt[i * 3 + 0];
            cur[i * 3 + 1] = nxt[i * 3 + 1];
            cur[i * 3 + 2] = nxt[i * 3 + 2];
        }
    }
    for (size_t i = 0; i < out.rgb.size(); ++i) {
        out.rgb[i] = static_cast<uint8_t>(std::min(255.f, std::max(0.f, cur[i] + 0.5f)));
    }
    for (size_t i = 0; i < out.mask.size(); ++i) out.mask[i] = 0;
    for (int y = 0; y < h; ++y) {
        std::memcpy(rgb + static_cast<size_t>(y) * rgb_stride,
                    out.rgb.data() + static_cast<size_t>(y) * w * 3,
                    static_cast<size_t>(w) * 3);
        std::memcpy(mask + static_cast<size_t>(y) * mask_stride,
                    out.mask.data() + static_cast<size_t>(y) * w, static_cast<size_t>(w));
    }
    return before - remaining_hole(out);
}

}  // namespace sv
