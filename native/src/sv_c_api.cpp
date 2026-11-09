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
#include "sv/manifest.h"
#include "sv/memory.h"
#include "sv/segmenter.h"
#include "sv/synthetic_backend.h"
#if SV_WITH_ORT
#include "sv/ort_backend.h"
#endif

#define SV_LOG_TAG "ShadowVision"

namespace {

std::mutex g_mu;
std::unique_ptr<sv::LaMaInpainter> g_inpainter;
std::string g_version = "0.2-product abi2";
std::atomic<bool> g_loaded{false};

std::unique_ptr<sv::SegmentationSession> g_seg;
std::string g_seg_source = "none";
sv::Catalog g_catalog;
std::string g_catalog_error = "not loaded";

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

int64_t sv_rss_kb(void) { return sv::current_rss_kb(); }

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

/* ------------------------------- Segments --------------------------------- */

int32_t sv_load_segmenter(const char* encoder_path, const char* decoder_path) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_seg.reset();
    const bool synthetic = (encoder_path == nullptr || encoder_path[0] == '\0');
    if (synthetic) {
        g_seg.reset(new sv::SegmentationSession(sv::CreateSyntheticEncoder(),
                                                sv::CreateSyntheticDecoder()));
        g_seg_source = "built-in";
    } else {
        // ONNX segmenter implementation lives in ort_segmenter.cpp behind
        // SV_WITH_ORT; until then the built-in one keeps the studio usable.
        __android_log_print(ANDROID_LOG_WARN, SV_LOG_TAG,
                            "onnx segmenter not built in this configuration; using built-in");
        g_seg.reset(new sv::SegmentationSession(sv::CreateSyntheticEncoder(),
                                                sv::CreateSyntheticDecoder()));
        g_seg_source = "built-in (onnx unavailable)";
    }
    sv::BackendOptions enc;
    sv::BackendOptions dec;
    if (encoder_path) enc.model_path = encoder_path;
    if (decoder_path) dec.model_path = decoder_path;
    const sv::Status st = g_seg->load(enc, dec);
    if (st != sv::Status::Ok) {
        g_seg.reset();
        return to_c(st);
    }
    __android_log_print(ANDROID_LOG_INFO, SV_LOG_TAG, "segmenter loaded: %s", g_seg_source.c_str());
    return SV_OK;
}

int32_t sv_seg_prepare(const uint8_t* rgba, int32_t width, int32_t height) {
    if (!rgba || width <= 0 || height <= 0) return SV_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_seg) return SV_ERR_MODEL_NOT_LOADED;
    const size_t px = static_cast<size_t>(width) * height;
    std::vector<uint8_t> rgb(px * 3);
    for (size_t i = 0; i < px; ++i) {
        rgb[i * 3 + 0] = rgba[i * 4 + 0];
        rgb[i * 3 + 1] = rgba[i * 4 + 1];
        rgb[i * 3 + 2] = rgba[i * 4 + 2];
    }
    const sv::Status st =
        g_seg->prepare(sv::ImageRGB{rgb.data(), width, height, width * 3});
    __android_log_print(ANDROID_LOG_INFO, SV_LOG_TAG, "seg encoder %.1f ms -> %s",
                        g_seg->encoder_ms(), sv::ToString(st));
    return to_c(st);
}

int32_t sv_seg_tap(const int32_t* xs, const int32_t* ys, const int8_t* labels, int32_t count,
                   uint8_t* out_mask, int32_t* out_w, int32_t* out_h) {
    if (!xs || !ys || !labels || count <= 0 || !out_mask || !out_w || !out_h) {
        return SV_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_seg) return SV_ERR_MODEL_NOT_LOADED;
    std::vector<sv::PointPrompt> prompts;
    prompts.reserve(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i) {
        prompts.push_back(sv::PointPrompt{static_cast<float>(xs[i]), static_cast<float>(ys[i]),
                                          labels[i] != 0 ? 1 : 0});
    }
    std::vector<uint8_t> mask;
    int mw = 0, mh = 0;
    const sv::Status st = g_seg->select(prompts, &mask, &mw, &mh);
    if (st != sv::Status::Ok) return to_c(st);
    std::memcpy(out_mask, mask.data(), mask.size());
    *out_w = mw;
    *out_h = mh;
    __android_log_print(ANDROID_LOG_INFO, SV_LOG_TAG, "seg decoder %.1f ms (%d pts)",
                        g_seg->decoder_ms(), count);
    return SV_OK;
}

void sv_seg_timings(float* encoder_ms, float* decoder_ms) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (encoder_ms) *encoder_ms = g_seg ? g_seg->encoder_ms() : 0.f;
    if (decoder_ms) *decoder_ms = g_seg ? g_seg->decoder_ms() : 0.f;
}

void sv_seg_release(void) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_seg) g_seg->release();
    g_seg.reset();
}

/* -------------------------------- Catalog --------------------------------- */

int32_t sv_catalog_load(const char* manifest_path, const char* pubkey_hex) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_catalog = sv::Catalog{};
    if (!manifest_path || !pubkey_hex) {
        g_catalog_error = "manifest path and public key are required";
        return SV_ERR_INVALID_ARGUMENT;
    }
    FILE* f = fopen(manifest_path, "rb");
    if (!f) {
        g_catalog_error = "catalog file not found";
        return SV_ERR_MODEL_NOT_LOADED;
    }
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);

    if (!sv::VerifyAndParseCatalog(text, pubkey_hex, &g_catalog)) {
        g_catalog_error = g_catalog.reason;
        __android_log_print(ANDROID_LOG_ERROR, SV_LOG_TAG, "catalog rejected: %s",
                            g_catalog.reason.c_str());
        return SV_ERR_INVALID_ARGUMENT;
    }
    g_catalog_error = "ok";
    __android_log_print(ANDROID_LOG_INFO, SV_LOG_TAG, "catalog verified: %zu entries, key %s",
                        g_catalog.models.size(), g_catalog.key_id.c_str());
    return SV_OK;
}

const char* sv_catalog_error(void) { return g_catalog_error.c_str(); }

const char* sv_catalog_json(void) {
    // A tiny, dependency-free JSON writer. The payload is small and the schema is
    // fixed, so pulling in a JSON library for it would be pure weight.
    static std::string json;
    json.clear();
    json += "[";
    for (size_t i = 0; i < g_catalog.models.size(); ++i) {
        const sv::ModelEntry& m = g_catalog.models[i];
        if (i) json += ",";
        char num[64];
        json += "{\"id\":\"" + m.id + "\",\"file\":\"" + m.file + "\"";
        snprintf(num, sizeof(num), ",\"bytes\":%lld", static_cast<long long>(m.bytes));
        json += num;
        json += ",\"license\":\"" + m.license + "\"";
        json += m.commercial ? ",\"commercial\":true" : ",\"commercial\":false";
        snprintf(num, sizeof(num), ",\"input_w\":%d,\"input_h\":%d", m.input_w, m.input_h);
        json += num;
        json += ",\"params\":\"" + (m.params.empty() ? std::string("none") : m.params) + "\"";
        json += ",\"steps\":[";
        for (size_t k = 0; k < m.steps.size(); ++k) {
            if (k) json += ",";
            snprintf(num, sizeof(num), "%d", m.steps[k]);
            json += num;
        }
        json += "],\"cfg\":[";
        for (size_t k = 0; k < m.cfg.size(); ++k) {
            if (k) json += ",";
            snprintf(num, sizeof(num), "%.2f", m.cfg[k]);
            json += num;
        }
        json += "],";
        json += m.seed_supported ? "\"seed\":true}" : "\"seed\":false}";
    }
    json += "]";
    return json.c_str();
}

int32_t sv_catalog_verify_file(const char* models_dir, const char* model_id, char* detail,
                               int32_t detail_len) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_catalog.verified) {
        if (detail && detail_len > 0) snprintf(detail, detail_len, "catalog not verified");
        return SV_ERR_MODEL_NOT_LOADED;
    }
    const sv::ModelEntry* e = model_id ? g_catalog.find(model_id) : nullptr;
    if (!e) {
        if (detail && detail_len > 0) snprintf(detail, detail_len, "unknown model id");
        return SV_ERR_INVALID_ARGUMENT;
    }
    std::string why;
    const sv::Status st = sv::VerifyModelFile(models_dir ? models_dir : "", *e, &why);
    if (detail && detail_len > 0) snprintf(detail, detail_len, "%s", why.c_str());
    return to_c(st);
}

}  // extern "C"
