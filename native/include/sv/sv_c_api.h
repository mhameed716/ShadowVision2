/* SPDX-License-Identifier: MIT
 * ShadowVision2 — stable C ABI (the ONLY contract Kotlin sees).
 *
 * Rules (docs/design-review-ar.md §4.2):
 *   - extern "C", no C++ types in the signature, no exceptions across the line.
 *   - Every call is safe to make from any thread; the implementation serialises
 *     on one worker. Returns an int status (see SvStatus).
 *   - The native side never calls back into Java except for progress, and only
 *     when the caller installed a callback.
 *   - Struct layout is versioned by SV_ABI_VERSION; bump it on any change.
 */
#ifndef SV_C_API_H
#define SV_C_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SV_ABI_VERSION 2

typedef enum SvStatus {
    SV_OK = 0,
    SV_ERR_INVALID_ARGUMENT = 1,
    SV_ERR_OUT_OF_MEMORY = 2,
    SV_ERR_BUDGET_EXCEEDED = 3,
    SV_ERR_MODEL_NOT_LOADED = 4,
    SV_ERR_BACKEND_UNAVAILABLE = 5,
    SV_ERR_CANCELLED = 6,
    SV_ERR_THERMAL_THROTTLED = 7,
    SV_ERR_INTERNAL = 8
} SvStatus;

typedef struct SvStage {
    const char* name;      /* static string, valid for process lifetime */
    double ms;
    int64_t peak_rss_kb;
} SvStage;

typedef struct SvReport {
    int32_t status;             /* SvStatus */
    double total_ms;
    double first_pixel_ms;      /* -1 if not measured */
    int64_t peak_rss_kb;        /* VmHWM */
    int64_t tracked_peak_kb;    /* MemoryGovernor peak */
    int64_t budget_kb;
    int32_t thermal_status;     /* PowerManager thermal status, -1 unknown */
    float npu_partition_ratio;  /* 0..1, -1 unknown */
    int32_t stage_count;
    SvStage stages[8];
} SvReport;

/* Lifecycle. One global engine per process; sessions are cheap. */
int32_t sv_init(int64_t budget_kb);
void sv_shutdown(void);
const char* sv_version(void);

/* Current process RSS in KB (VmRSS), or -1. Cheap; the console polls it ~1/s. */
int64_t sv_rss_kb(void);

/* Load an inpainting model. `model_path` may be empty to select the synthetic
 * backend (used by the harness to validate plumbing on-device). */
int32_t sv_load_model(const char* model_path, int32_t use_npu, int32_t threads);

/* Inpaint in place on an RGBA_8888 buffer (Android Bitmap layout: R,G,B,A).
 * The call is synchronous and must be made from a worker thread.
 * `cancel_flag` is polled between stages; pass NULL to disable cancellation. */
int32_t sv_inpaint_rgba(uint8_t* rgba, int32_t width, int32_t height,
                        const uint8_t* mask, int32_t mask_width, int32_t mask_height,
                        int32_t work_resolution, const int32_t* cancel_flag,
                        SvReport* out_report);

/* Releases weights and scratch buffers. Called when leaving the editor. */
void sv_release(void);

/* ---------------- Segmentation (MobileSAM, split encoder/decoder) ----------- */

/* Loads encoder + decoder from signed-catalog paths. Empty paths select the
 * built-in synthetic segmenter (no model installed), which keeps the studio
 * usable and testable on a fresh install. */
int32_t sv_load_segmenter(const char* encoder_path, const char* decoder_path);

/* Encodes the image once (0.4-1.2 s). Call after every image change. */
int32_t sv_seg_prepare(const uint8_t* rgba, int32_t width, int32_t height);

/* Answers a tap/box prompt. Writes a full-resolution 8-bit mask (0/255) into
 * `out_mask` (must hold width*height bytes) and returns the mask dimensions via
 * out_w/out_h. Prompts accumulate: label 1 = keep, 0 = exclude. */
int32_t sv_seg_tap(const int32_t* xs, const int32_t* ys, const int8_t* labels, int32_t count,
                   uint8_t* out_mask, int32_t* out_w, int32_t* out_h);

/* Encoder/decoder timings of the last operations, in ms. */
void sv_seg_timings(float* encoder_ms, float* decoder_ms);

void sv_seg_release(void);

/* ------------------------------- Catalog ---------------------------------- */

/* Verifies the Ed25519 signature of a catalog file against `pubkey_hex`
 * (64 hex chars = 32 bytes) and caches the parsed entries.
 * Returns SV_OK, or an error with the reason available via sv_catalog_error(). */
int32_t sv_catalog_load(const char* manifest_path, const char* pubkey_hex);
const char* sv_catalog_error(void);

/* The verified catalog as JSON: [{id,file,bytes,license,commercial,input_w,
 * input_h,params,steps,cfg,seed}]. Empty array when nothing is verified. */
const char* sv_catalog_json(void);

/* Size + SHA-256 check of one catalog file inside models_dir.
 * Returns SV_OK, SV_ERR_MODEL_NOT_LOADED (missing) or SV_ERR_INVALID_ARGUMENT
 * (mismatch); `detail` receives a human-readable reason. */
int32_t sv_catalog_verify_file(const char* models_dir, const char* model_id, char* detail,
                               int32_t detail_len);

/* Log ring buffer (the "live logs" panel reads this, never logcat parsing). */
int32_t sv_log_dump(char* buf, int32_t buf_len);
int32_t sv_log_capacity(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SV_C_API_H */
