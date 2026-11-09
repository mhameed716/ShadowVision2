// SPDX-License-Identifier: MIT
#include "sv/synthetic_backend.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "sv/memory.h"

namespace sv {

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
    // Pyramid fill, not eight ring passes: eight passes leave every hole wider
    // than ~8 px with its original pixels in the middle, and this backend still
    // returned Status::Ok.
    const int filled = PyramidFill(work.data(), image.width * 3, m.data(), image.width,
                                   image.width, image.height);
    // Whatever survives is a hole the geometry could not reach (a hole touching
    // the frame edge, or a level that was entirely hole). Reporting Ok while the
    // object is still on screen is the failure mode this whole change is about,
    // so it is an error, not a warning nobody reads.
    int leftover = 0;
    for (uint8_t v : m) leftover += (v != 0);
    if (leftover > 0) {
        // No logger in the core (the console reads structured records), so this
        // goes to stderr on the host and to logcat on device, where the harness
        // picks it up. Silence here is what let "Ok with a hole still in it" ship.
        fprintf(stderr, "sv: synthetic fill left %d hole pixels (%d filled)\n", leftover,
                filled);
        return Status::Internal;
    }
    for (int y = 0; y < out.height; ++y) {
        memcpy(out.data + static_cast<size_t>(y) * out.stride,
               work.data() + static_cast<size_t>(y) * image.width * 3,
               static_cast<size_t>(out.width) * 3);
    }
    return Status::Ok;
}

}  // namespace sv
