// SPDX-License-Identifier: MIT
// ShadowVision2 — G0 core: common types.
//
// Design rules enforced here (see docs/decisions-ar.md):
//   * No exceptions in hot paths, no RTTI requirement for the ABI.
//   * Every buffer allocation goes through MemoryGovernor (sv::mem).
//   * Everything measurable is reported through Stats, never guessed.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace sv {

// ---------------------------------------------------------------------------
// Image / tensor views. Non-owning: the owner is always explicit.
// ---------------------------------------------------------------------------

// Interleaved RGB, 8-bit, row-major, stride = width * 3 (unless stated).
struct ImageRGB {
    const uint8_t* data = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;

    bool valid() const { return data != nullptr && width > 0 && height > 0; }
    size_t bytes() const { return static_cast<size_t>(stride) * static_cast<size_t>(height); }
};

// Single-channel mask, 8-bit (0 = keep, 255 = inpaint), row-major.
struct Mask {
    const uint8_t* data = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;

    bool valid() const { return data != nullptr && width > 0 && height > 0; }
    size_t bytes() const { return static_cast<size_t>(stride) * static_cast<size_t>(height); }
};

struct MutableImageRGB {
    uint8_t* data = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;

    bool valid() const { return data != nullptr && width > 0 && height > 0; }
};

// Result of an operation. Cheap, copyable, loggable.
enum class Status : uint8_t {
    Ok = 0,
    InvalidArgument,
    OutOfMemory,
    BudgetExceeded,   // MemoryGovernor refused the allocation
    ModelNotLoaded,
    BackendUnavailable,
    Cancelled,
    ThermalThrottled,
    Internal,
};

const char* ToString(Status s);

// ---------------------------------------------------------------------------
// Cancellation: cooperative, checked between steps and inside hot loops.
// The denoise loop lives in C++ precisely so this flag can be honoured.
// ---------------------------------------------------------------------------
class CancelToken {
public:
    void request() noexcept { m_cancelled.store(true, std::memory_order_relaxed); }
    void reset() noexcept { m_cancelled.store(false, std::memory_order_relaxed); }
    bool cancelled() const noexcept { return m_cancelled.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> m_cancelled{false};
};

// ---------------------------------------------------------------------------
// Stats: the only currency this project accepts.
// Populated by every stage; consumed by the on-device harness and the log.
// ---------------------------------------------------------------------------
struct StageTiming {
    const char* name = "";      // static string, never freed
    double ms = 0.0;
    int64_t peak_rss_kb = 0;    // peak RSS observed after the stage (Android: /proc/self/status)
};

struct Stats {
    double total_ms = 0.0;
    double first_pixel_ms = -1.0;   // time to first visible preview
    StageTiming stages[8] = {};
    int stage_count = 0;
    int64_t peak_rss_kb = 0;
    int64_t budget_kb = 0;
    int thermal_status = -1;        // PowerManager.THERMAL_STATUS_*
    float npu_partition_ratio = -1.f;  // fraction of nodes claimed by the NPU, -1 = unknown
    int retries = 0;

    void add(const char* stage_name, double stage_ms) {
        if (stage_count < 8) {
            stages[stage_count].name = stage_name;
            stages[stage_count].ms = stage_ms;
            stages[stage_count].peak_rss_kb = peak_rss_kb;
            ++stage_count;
        }
        total_ms += stage_ms;
    }
};

}  // namespace sv
