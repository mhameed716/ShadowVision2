// SPDX-License-Identifier: MIT
#include "sv/synthetic_backend.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#include "sv/memory.h"

namespace sv {

namespace {

// One pass: for each hole pixel, average the known pixels in its 3x3 (8-neighbour)
// ring weighted by inverse distance; newly filled pixels become "known" for the
// next pass. Deterministic, no randomness, no dependence on traversal order
// beyond the ring which is fixed.
int fill_pass(uint8_t* rgb, int rgb_stride, uint8_t* mask, int mask_stride, int w, int h) {
    std::vector<uint8_t> updates(static_cast<size_t>(w) * h * 4, 0);  // rgb + filled flag
    int filled = 0;
    static const int dx[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
    static const int dy[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
    for (int y = 1; y < h - 1; ++y) {
        const uint8_t* mrow = mask + static_cast<size_t>(y) * mask_stride;
        for (int x = 1; x < w - 1; ++x) {
            if (mrow[x] == 0) continue;
            float sr = 0, sg = 0, sb = 0, sw = 0;
            for (int k = 0; k < 8; ++k) {
                const int nx = x + dx[k];
                const int ny = y + dy[k];
                if (mask[static_cast<size_t>(ny) * mask_stride + nx] != 0) continue;
                const float wgt = (k == 0 || k == 2 || k == 5 || k == 7) ? 0.7071f : 1.0f;
                const uint8_t* p = rgb + static_cast<size_t>(ny) * rgb_stride + static_cast<size_t>(nx) * 3;
                sr += p[0] * wgt;
                sg += p[1] * wgt;
                sb += p[2] * wgt;
                sw += wgt;
            }
            if (sw <= 0.f) continue;
            const size_t idx = (static_cast<size_t>(y) * w + x) * 4;
            updates[idx + 0] = static_cast<uint8_t>(sr / sw);
            updates[idx + 1] = static_cast<uint8_t>(sg / sw);
            updates[idx + 2] = static_cast<uint8_t>(sb / sw);
            updates[idx + 3] = 1;
            ++filled;
        }
    }
    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            const size_t idx = (static_cast<size_t>(y) * w + x) * 4;
            if (!updates[idx + 3]) continue;
            uint8_t* p = rgb + static_cast<size_t>(y) * rgb_stride + static_cast<size_t>(x) * 3;
            p[0] = updates[idx + 0];
            p[1] = updates[idx + 1];
            p[2] = updates[idx + 2];
            mask[static_cast<size_t>(y) * mask_stride + x] = 0;
        }
    }
    return filled;
}

}  // namespace

void TeleaFill(uint8_t* rgb, int rgb_stride, uint8_t* mask, int mask_stride, int w, int h,
               int iterations) {
    for (int i = 0; i < iterations; ++i) {
        if (fill_pass(rgb, rgb_stride, mask, mask_stride, w, h) == 0) break;
    }
}

SyntheticBackend::SyntheticBackend(Config cfg) : m_cfg(cfg) {}

SyntheticBackend::SyntheticBackend() : m_cfg() {}

Status SyntheticBackend::load(const BackendOptions& opts) {
    (void)opts;
    m_ready = true;
    return Status::Ok;
}

void SyntheticBackend::release() { m_ready = false; }

Status SyntheticBackend::run(const ImageRGB& image, const Mask& mask, MutableImageRGB out) {
    if (!m_ready) return Status::ModelNotLoaded;
    if (!image.valid() || !mask.valid() || !out.valid()) return Status::InvalidArgument;
    if (m_cfg.fail_run) return Status::Internal;
    if (m_cfg.emulated_ns_per_pixel > 0.0) {
        const auto px = static_cast<double>(image.width) * image.height;
        const auto ns = static_cast<long long>(px * m_cfg.emulated_ns_per_pixel);
        std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
    }

    // Work in place on a copy; out is a full-frame buffer of the same size.
    const size_t bytes = static_cast<size_t>(image.width) * image.height * 3;
    if (m_cfg.fail_alloc) {
        auto lease = MemoryGovernor::global().acquire("synthetic.fail", bytes);
        if (!lease.live) return Status::BudgetExceeded;
    }
    auto lease = MemoryGovernor::global().acquire("synthetic.work", bytes);
    if (!lease.live) return Status::BudgetExceeded;

    std::vector<uint8_t> work(bytes);
    for (int y = 0; y < image.height; ++y) {
        memcpy(work.data() + static_cast<size_t>(y) * image.width * 3,
               image.data + static_cast<size_t>(y) * image.stride,
               static_cast<size_t>(image.width) * 3);
    }
    std::vector<uint8_t> m(static_cast<size_t>(image.width) * image.height);
    for (int y = 0; y < mask.height; ++y) {
        memcpy(m.data() + static_cast<size_t>(y) * image.width,
               mask.data + static_cast<size_t>(y) * mask.stride,
               static_cast<size_t>(mask.width));
    }
    TeleaFill(work.data(), image.width * 3, m.data(), image.width, image.width, image.height, 8);
    for (int y = 0; y < out.height; ++y) {
        memcpy(out.data + static_cast<size_t>(y) * out.stride,
               work.data() + static_cast<size_t>(y) * image.width * 3,
               static_cast<size_t>(out.width) * 3);
    }
    return Status::Ok;
}

}  // namespace sv
