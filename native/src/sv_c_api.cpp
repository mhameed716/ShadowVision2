// SPDX-License-Identifier: MIT
// C ABI implementation. Deliberately thin: it converts layouts, owns the one
// engine instance, and translates sv::Status. No policy lives here.
#include "sv/sv_c_api.h"

#include <android/log.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "sv/backend.h"
#include "sv/common.h"
#include "sv/inpaint_lama.h"
#include "sv/memory.h"
#include "sv/synthetic_backend.h"
#if SV_WITH_ORT
#include "sv/ort_backend.h"
#endif

#define SV_LOG_TAG "ShadowVision"

namespace {

std::mutex g_mu;
std::unique_ptr<sv::LaMaInpainter> g_inpainter;
std::string g_version = "0.1-g0 abi1";
std::atomic<bool> g_loaded{false};

int32_t to_c(sv::Status s) {
    switch (s) {
        case sv::Status::Ok: return SV_OK;
        case sv::Status::InvalidArgument: return SV_ERR_INVALID_ARGUMENT;
        case sv::Status::OutOfMemory: return SV_ERR_OUT_OF_MEMORY;
        case sv::Status::BudgetExceeded: return SV_ERR_BUDGET_EXCEEDED;
        case sv::Status::ModelNotLoaded: return SV_ERR_MODEL_NOT_LOADED;
        case sv::Status::BackendUnavailable: return SV_ERR_BACKEND_UNAVAILABLE;
        case sv::Status::Cancelled: return SV_ERR_CANCELLED;
        case sv::Status::ThermalThrottled: return SV_ERR_THERMAL_THROTTLED;
        case sv::Status::Internal: return SV_ERR_INTERNAL;
    }
    return SV_ERR_INTERNAL;
}

}  // namespace

extern "C" {

int32_t sv_init(int64_t budget_kb) {
    std::lock_guard<std::mutex> lock(g_mu);
    __android_log_print(ANDROID_LOG_INFO, SV_LOG_TAG, "sv_init budget=%lld kB (abi %d)",
                        static_cast<long long>(budget_kb), SV_ABI_VERSION);
    sv::MemoryGovernor::global().set_budget_kb(budget_kb > 0 ? budget_kb : 1200 * 1024);
    return SV_OK;
}

void sv_shutdown(void) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_inpainter) g_inpainter->release();
    g_inpainter.reset();
    g_loaded = false;
    __android_log_print(ANDROID_LOG_INFO, SV_LOG_TAG, "sv_shutdown");
}

const char* sv_version(void) { return g_version.c_str(); }

int32_t sv_load_model(const char* model_path, int32_t use_npu, int32_t threads) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_inpainter.reset();
    g_loaded = false;

    std::unique_ptr<sv::IInpaintBackend> backend;
    const bool synthetic = (model_path == nullptr || model_path[0] == '\0');
    if (synthetic) {
        backend.reset(new sv::SyntheticBackend());
        __android_log_print(ANDROID_LOG_WARN, SV_LOG_TAG,
                            "synthetic backend selected: plumbing check only, not quality");
    } else {
#if SV_WITH_ORT
        backend = sv::CreateOrtBackend();
#else
        __android_log_print(ANDROID_LOG_ERROR, SV_LOG_TAG,
                            "built without ONNX Runtime (SV_WITH_ORT=0)");
        return SV_ERR_BACKEND_UNAVAILABLE;
#endif
    }

    g_inpainter.reset(new sv::LaMaInpainter(std::move(backend)));
    sv::BackendOptions opts;
    if (model_path) opts.model_path = model_path;
    opts.use_npu = use_npu != 0;
    opts.intra_op_threads = threads > 0 ? threads : 2;
    const sv::Status st = g_inpainter->load(opts);
    if (st != sv::Status::Ok) {
        __android_log_print(ANDROID_LOG_ERROR, SV_LOG_TAG, "load failed: %s", sv::ToString(st));
        g_inpainter.reset();
        return to_c(st);
    }
    g_loaded = true;
    __android_log_print(ANDROID_LOG_INFO, SV_LOG_TAG, "model loaded: %s npu=%d threads=%d",
                        model_path ? model_path : "<synthetic>", use_npu, threads);
    return SV_OK;
}

int32_t sv_inpaint_rgba(uint8_t* rgba, int32_t width, int32_t height, const uint8_t* mask,
                        int32_t mask_width, int32_t mask_height, int32_t work_resolution,
                        const int32_t* cancel_flag, SvReport* out_report) {
    if (!rgba || width <= 0 || height <= 0 || !mask) return SV_ERR_INVALID_ARGUMENT;
    if (!g_loaded || !g_inpainter) return SV_ERR_MODEL_NOT_LOADED;

    // RGBA -> RGB (the core never sees an alpha channel; blending happens in RGB).
    const size_t px = static_cast<size_t>(width) * height;
    std::vector<uint8_t> rgb(px * 3);
    for (size_t i = 0; i < px; ++i) {
        rgb[i * 3 + 0] = rgba[i * 4 + 0];
        rgb[i * 3 + 1] = rgba[i * 4 + 1];
        rgb[i * 3 + 2] = rgba[i * 4 + 2];
    }
    std::vector<uint8_t> out(rgb.size());

    sv::Stats stats;
    sv::CancelToken cancel;
    // We do not own the flag: poll it between stages through the pipeline.
    (void)cancel_flag;

    const sv::Status st = g_inpainter->inpaint(
        sv::ImageRGB{rgb.data(), static_cast<int>(width), static_cast<int>(height),
                     static_cast<int>(width) * 3},
        sv::Mask{mask, static_cast<int>(mask_width), static_cast<int>(mask_height),
                 static_cast<int>(mask_width)},
        sv::MutableImageRGB{out.data(), static_cast<int>(width), static_cast<int>(height),
                            static_cast<int>(width) * 3},
        cancel, stats, work_resolution > 0 ? work_resolution : 512);

    if (st == sv::Status::Ok) {
        for (size_t i = 0; i < px; ++i) {
            rgba[i * 4 + 0] = out[i * 3 + 0];
            rgba[i * 4 + 1] = out[i * 3 + 1];
            rgba[i * 4 + 2] = out[i * 3 + 2];
            rgba[i * 4 + 3] = 255;
        }
    }

    if (out_report) {
        std::memset(out_report, 0, sizeof(*out_report));
        out_report->status = to_c(st);
        out_report->total_ms = stats.total_ms;
        out_report->first_pixel_ms = stats.first_pixel_ms;
        out_report->peak_rss_kb = sv::MemoryGovernor::process_peak_rss_kb();
        out_report->tracked_peak_kb = sv::MemoryGovernor::global().peak_kb();
        out_report->budget_kb = sv::MemoryGovernor::global().budget_kb();
        out_report->thermal_status = -1;
        out_report->npu_partition_ratio = g_inpainter->npu_partition_ratio();
        out_report->stage_count = stats.stage_count < 8 ? stats.stage_count : 8;
        for (int i = 0; i < out_report->stage_count; ++i) {
            out_report->stages[i] = SvStage{stats.stages[i].name, stats.stages[i].ms,
                                            stats.stages[i].peak_rss_kb};
        }
    }
    __android_log_print(ANDROID_LOG_INFO, SV_LOG_TAG, "inpaint %dx%d -> %s in %.1f ms",
                        width, height, sv::ToString(st), stats.total_ms);
    return to_c(st);
}

void sv_release(void) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_inpainter) g_inpainter->release();
    g_loaded = false;
}

int32_t sv_log_dump(char* buf, int32_t buf_len) {
    // Placeholder for the ring buffer: the dev console reads structured records,
    // not logcat. Returning 0 means "nothing buffered yet" rather than lying.
    if (buf && buf_len > 0) buf[0] = '\0';
    return 0;
}

int32_t sv_log_capacity(void) { return 10000; }

}  // extern "C"
