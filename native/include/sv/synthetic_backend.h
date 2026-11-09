// SPDX-License-Identifier: MIT
// Synthetic inpainting backend.
//
// Purpose: make the entire G0 harness (crop, budget, blend, stats, thresholds,
// failure injection) executable on CI and on any laptop with no weights, no
// NDK and no phone. It is a deterministic "telea-lite": fill each hole pixel
// with a distance-weighted average of the nearest known pixels, then add a
// controllable amount of work to emulate inference cost.
//
// It is NOT a quality backend. It exists so that the only thing left unproven
// after `ctest` is the network itself.
#pragma once

#include <cstdint>
#include <vector>

#include "sv/backend.h"

namespace sv {

class SyntheticBackend : public IInpaintBackend {
public:
    struct Config {
        // Emulated per-pixel cost in nanoseconds, per megapixel, so that timings
        // look like a real phone. 0 = as fast as the machine allows.
        double emulated_ns_per_pixel = 0.0;
        // Inject a BudgetExceeded failure to prove the governor path is wired.
        bool fail_alloc = false;
        // Emulate an empty/failed run to exercise error propagation.
        bool fail_run = false;
    };

    explicit SyntheticBackend(Config cfg);
    SyntheticBackend();
    ~SyntheticBackend() override = default;

    Status load(const BackendOptions& opts) override;
    Status run(const ImageRGB& image, const Mask& mask, MutableImageRGB out) override;
    const char* name() const override { return "synthetic"; }
    bool ready() const override { return m_ready; }
    float npu_partition_ratio() const override { return 0.f; }  // honest: no NPU
    void release() override;

private:
    Config m_cfg;
    bool m_ready = false;
};

// Multi-scale Telea-style fill, exposed for unit tests and for the L0 preview
// layer. `iterations` controls how far information propagates per pass; 3-4
// passes are plenty at 1/4 preview resolution.
void TeleaFill(uint8_t* rgb, int rgb_stride, uint8_t* mask, int mask_stride, int w, int h,
               int iterations);

}  // namespace sv
