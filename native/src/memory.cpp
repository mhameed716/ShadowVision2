// SPDX-License-Identifier: MIT
#include "sv/memory.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#if defined(__linux__) || defined(__ANDROID__)
#include <unistd.h>
#endif

namespace sv {

namespace {
int64_t read_proc_status_kb(const char* key) {
#if defined(__linux__) || defined(__ANDROID__)
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    int64_t value = -1;
    const size_t key_len = strlen(key);
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, key_len) == 0) {
            // "VmHWM:\t  123456 kB"
            long long kb = 0;
            if (sscanf(line + key_len, " %lld", &kb) == 1) value = static_cast<int64_t>(kb);
            break;
        }
    }
    fclose(f);
    return value;
#else
    (void)key;
    return -1;
#endif
}
}  // namespace

int64_t current_rss_kb() { return read_proc_status_kb("VmRSS:"); }
int64_t MemoryGovernor::process_peak_rss_kb() { return read_proc_status_kb("VmHWM:"); }

MemoryGovernor& MemoryGovernor::global() {
    // 1.2 GB default: comfortable on 6 GB devices, honest for a 512x512 pipeline.
    static MemoryGovernor g(1200 * 1024);
    return g;
}

MemoryGovernor::MemoryGovernor(int64_t budget_kb) : m_budget_kb(budget_kb) {}

Status MemoryGovernor::reserve(const char* label, size_t bytes) {
    const int64_t kb = static_cast<int64_t>((bytes + 1023) / 1024);
    std::lock_guard<std::mutex> lock(m_mu);
    if (m_current_kb + kb > m_budget_kb) return Status::BudgetExceeded;
    m_current_kb += kb;
    if (m_current_kb > m_peak_kb) m_peak_kb = m_current_kb;
    (void)label;
    return Status::Ok;
}

void MemoryGovernor::release(const char* label, size_t bytes) {
    const int64_t kb = static_cast<int64_t>((bytes + 1023) / 1024);
    std::lock_guard<std::mutex> lock(m_mu);
    m_current_kb -= kb;
    if (m_current_kb < 0) m_current_kb = 0;  // never go negative: a bug, not a state
    (void)label;
}

int64_t MemoryGovernor::current_kb() const noexcept {
    std::lock_guard<std::mutex> lock(m_mu);
    return m_current_kb;
}

int64_t MemoryGovernor::peak_kb() const noexcept {
    std::lock_guard<std::mutex> lock(m_mu);
    return m_peak_kb;
}

int64_t MemoryGovernor::budget_kb() const noexcept {
    std::lock_guard<std::mutex> lock(m_mu);
    return m_budget_kb;
}

void MemoryGovernor::set_budget_kb(int64_t kb) {
    std::lock_guard<std::mutex> lock(m_mu);
    m_budget_kb = kb;
}

std::string MemoryGovernor::dump() const {
    std::lock_guard<std::mutex> lock(m_mu);
    char buf[128];
    snprintf(buf, sizeof(buf), "used=%lldkB peak=%lldkB budget=%lldkB",
             static_cast<long long>(m_current_kb), static_cast<long long>(m_peak_kb),
             static_cast<long long>(m_budget_kb));
    return std::string(buf);
}

MemoryGovernor::Lease::~Lease() {
    if (live) MemoryGovernor::global().release(label, bytes);
}

MemoryGovernor::Lease MemoryGovernor::acquire(const char* label, size_t bytes) {
    Lease lease(label, bytes);
    if (reserve(label, bytes) == Status::Ok) {
        lease.live = true;
    } else {
        fprintf(stderr, "[sv][mem] REFUSED %s (%zu bytes) — %s\n", label, bytes,
                dump().c_str());
    }
    return lease;
}

const char* ToString(Status s) {
    switch (s) {
        case Status::Ok: return "ok";
        case Status::InvalidArgument: return "invalid_argument";
        case Status::OutOfMemory: return "out_of_memory";
        case Status::BudgetExceeded: return "budget_exceeded";
        case Status::ModelNotLoaded: return "model_not_loaded";
        case Status::BackendUnavailable: return "backend_unavailable";
        case Status::Cancelled: return "cancelled";
        case Status::ThermalThrottled: return "thermal_throttled";
        case Status::Internal: return "internal";
    }
    return "unknown";
}

}  // namespace sv
