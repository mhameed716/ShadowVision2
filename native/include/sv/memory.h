// SPDX-License-Identifier: MIT
// MemoryGovernor — the single gate for large allocations.
//
// Why this exists (docs/decisions-ar.md, D9 rule 3):
//   Android kills the process (LMK) before malloc returns null. A C++ allocation
//   failure is therefore NOT the failure mode you will observe in the field; the
//   failure mode is "process silently disappears". The only defence is to refuse
//   allocations ourselves, before the kernel has to, and to know the peak.
//
// Contract:
//   * Every tensor / image / model buffer above a small threshold goes through
//     reserve()/release().
//   * reserve() returns BudgetExceeded instead of trying and dying.
//   * peak() is reported as KB so it lines up with /proc/self/status VmHWM.
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "sv/common.h"

namespace sv {

class MemoryGovernor {
public:
    // budget_kb is the hard ceiling for *tracked* allocations. Untracked small
    // allocations (strings, vectors of a few hundred bytes) are ignored on purpose.
    explicit MemoryGovernor(int64_t budget_kb);

    MemoryGovernor(const MemoryGovernor&) = delete;
    MemoryGovernor& operator=(const MemoryGovernor&) = delete;
    static MemoryGovernor& global();

    // Reserve `bytes` under a human-readable label. Fails with BudgetExceeded
    // if the total would exceed the budget. Thread-safe.
    Status reserve(const char* label, size_t bytes);

    // Release a reservation previously made with the same label+bytes.
    void release(const char* label, size_t bytes);

    int64_t current_kb() const noexcept;
    int64_t peak_kb() const noexcept;
    int64_t budget_kb() const noexcept;
    void set_budget_kb(int64_t kb);

    // Process-wide peak RSS in KB (VmHWM on Linux/Android, GetProcessMemoryInfo
    // on other platforms; -1 when unavailable). Not tracked memory: real memory.
    static int64_t process_peak_rss_kb();

    // Everything currently reserved, for the dev console. Format is stable:
    // "label=NNNkB;label=NNNkB".
    std::string dump() const;

    // A reservation is RAII: the label makes leaks attributable.
    struct Lease {
        Lease() = default;
        Lease(const char* label, size_t bytes) : label(label), bytes(bytes) {}
        Lease(Lease&& o) noexcept : label(o.label), bytes(o.bytes), live(o.live) { o.live = false; }
        Lease& operator=(Lease&&) = delete;
        Lease(const Lease&) = delete;
        ~Lease();
        const char* label = "";
        size_t bytes = 0;
        bool live = false;
    };

    Lease acquire(const char* label, size_t bytes);  // status via lease.live

private:
    mutable std::mutex m_mu;
    int64_t m_budget_kb;
    int64_t m_current_kb = 0;
    int64_t m_peak_kb = 0;
};

// Convenience: current-process RSS in KB (VmRSS), -1 if unavailable.
int64_t current_rss_kb();

}  // namespace sv
