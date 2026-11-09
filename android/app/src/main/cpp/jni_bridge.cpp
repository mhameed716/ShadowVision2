// SPDX-License-Identifier: MIT
// JNI glue. Two rules, both learned the expensive way:
//   1. RegisterNatives instead of name mangling: the symbols stay stable when
//      the package moves, and typos fail loudly at load time.
//   2. Nothing heavy happens here. GetByteArrayElements copies; the copy is the
//      call itself, so we keep the JNI surface at exactly 7 functions.
#include <jni.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "sv/sv_c_api.h"

namespace {

// MUST match the Kotlin object that declares the externals. It said
// "ai/shadowvision/g0/SvEngine" while the class was moved to core/: the
// library then failed to register natives and System.loadLibrary threw
// UnsatisfiedLinkError before the first screen drew. Caught by
// tools/check_jni_binding.py, which parses both sides.
constexpr const char* kClass = "ai/shadowvision/core/SvEngine";

void throw_illegal_state(JNIEnv* env, const char* msg) {
    if (!env) return;
    jclass cls = env->FindClass("java/lang/IllegalStateException");
    if (cls) env->ThrowNew(cls, msg);
}

jint JNICALL n_init(JNIEnv*, jclass, jlong budget_kb) {
    return sv_init(static_cast<int64_t>(budget_kb));
}

jint JNICALL n_load(JNIEnv* env, jclass, jstring path, jboolean use_npu, jint threads) {
    std::string p;
    if (path != nullptr) {
        const char* chars = env->GetStringUTFChars(path, nullptr);
        if (chars) {
            p = chars;
            env->ReleaseStringUTFChars(path, chars);
        }
    }
    return sv_load_model(p.empty() ? nullptr : p.c_str(), use_npu ? 1 : 0, threads);
}

jint JNICALL n_inpaint(JNIEnv* env, jclass, jbyteArray rgba, jint w, jint h, jbyteArray mask,
                       jint mw, jint mh, jint work, jintArray /*cancel*/, jlongArray report) {
    if (!rgba || !mask || !report) {
        throw_illegal_state(env, "null buffers");
        return SV_ERR_INVALID_ARGUMENT;
    }
    jsize rgba_len = env->GetArrayLength(rgba);
    jsize mask_len = env->GetArrayLength(mask);
    // Width and height are externals from Kotlin; the multiplication must be done
    // in 64-bit. `w * h * 4` overflows int32 above ~46340 px per side, and the
    // check then passes for a buffer that is far too small - a heap overflow
    // guarded by a comparison that silently wrapped.
    if (w <= 0 || h <= 0 || mw <= 0 || mh <= 0) {
        throw_illegal_state(env, "empty bitmap or mask");
        return SV_ERR_INVALID_ARGUMENT;
    }
    const int64_t need_rgba = static_cast<int64_t>(w) * h * 4;
    const int64_t need_mask = static_cast<int64_t>(mw) * mh;
    if (static_cast<int64_t>(rgba_len) < need_rgba ||
        static_cast<int64_t>(mask_len) < need_mask) {
        throw_illegal_state(env, "buffer sizes do not match the bitmap");
        return SV_ERR_INVALID_ARGUMENT;
    }

    auto* rgba_ptr = static_cast<uint8_t*>(env->GetPrimitiveArrayCritical(rgba, nullptr));
    if (!rgba_ptr) return SV_ERR_OUT_OF_MEMORY;
    std::vector<uint8_t> mask_copy(static_cast<size_t>(mw) * static_cast<size_t>(mh));
    env->GetByteArrayRegion(mask, 0, static_cast<jsize>(mask_copy.size()),
                            reinterpret_cast<jbyte*>(mask_copy.data()));

    SvReport rep;
    std::memset(&rep, 0, sizeof(rep));
    const int32_t st = sv_inpaint_rgba(rgba_ptr, w, h, mask_copy.data(), mw, mh, work, nullptr, &rep);
    env->ReleasePrimitiveArrayCritical(rgba, rgba_ptr, 0);

    // Fixed report layout — keep in sync with SvEngine.kt.
    jlong out[64];
    std::memset(out, 0, sizeof(out));
    out[0] = rep.status;
    out[1] = static_cast<jlong>(rep.total_ms * 1000.0);
    out[2] = rep.peak_rss_kb;
    out[3] = rep.tracked_peak_kb;
    out[4] = rep.budget_kb;
    out[5] = static_cast<jlong>(rep.npu_partition_ratio * 1000.f);
    out[6] = rep.stage_count;
    for (int i = 0; i < rep.stage_count && i < 8; ++i) {
        out[7 + i * 2] = static_cast<jlong>(rep.stages[i].ms * 1000.0);
        out[8 + i * 2] = rep.stages[i].peak_rss_kb;
    }
    env->SetLongArrayRegion(report, 0, 64, out);
    return st;
}

void JNICALL n_release(JNIEnv*, jclass) { sv_release(); }

jdouble JNICALL n_rss_mb(JNIEnv*, jclass) {
    const int64_t kb = sv_rss_kb();
    return kb > 0 ? static_cast<jdouble>(kb) / 1024.0 : -1.0;
}

/* ---- segmentation -------------------------------------------------------- */

jint JNICALL n_seg_load(JNIEnv* env, jclass, jstring enc, jstring dec) {
    auto to_str = [&](jstring s, std::string* out) {
        if (!s) return;
        const char* c = env->GetStringUTFChars(s, nullptr);
        if (c) {
            *out = c;
            env->ReleaseStringUTFChars(s, c);
        }
    };
    std::string e, d;
    to_str(enc, &e);
    to_str(dec, &d);
    return sv_load_segmenter(e.empty() ? nullptr : e.c_str(), d.empty() ? nullptr : d.c_str());
}

jint JNICALL n_seg_prepare(JNIEnv* env, jclass, jbyteArray rgba, jint w, jint h) {
    if (!rgba) return SV_ERR_INVALID_ARGUMENT;
    auto* ptr = static_cast<uint8_t*>(env->GetPrimitiveArrayCritical(rgba, nullptr));
    const jint rc = sv_seg_prepare(ptr, w, h);
    env->ReleasePrimitiveArrayCritical(rgba, ptr, JNI_ABORT);
    return rc;
}

jint JNICALL n_seg_tap(JNIEnv* env, jclass, jintArray xs, jintArray ys, jbyteArray labels,
                       jint count, jbyteArray out_mask, jintArray dims) {
    if (!xs || !ys || !labels || !out_mask || !dims) return SV_ERR_INVALID_ARGUMENT;
    jint px[16];
    jint py[16];
    jbyte pl[16];
    const jint n = count > 16 ? 16 : count;  // more than 16 taps is a UI bug, not a use case
    env->GetIntArrayRegion(xs, 0, n, px);
    env->GetIntArrayRegion(ys, 0, n, py);
    env->GetByteArrayRegion(labels, 0, n, pl);
    auto* mask = static_cast<uint8_t*>(env->GetPrimitiveArrayCritical(out_mask, nullptr));
    jint w = 0, h = 0;
    const jint rc = sv_seg_tap(px, py, reinterpret_cast<const int8_t*>(pl), n, mask, &w, &h);
    env->ReleasePrimitiveArrayCritical(out_mask, mask, 0);
    const jint d[2] = {w, h};
    env->SetIntArrayRegion(dims, 0, 2, d);
    return rc;
}

void JNICALL n_seg_release(JNIEnv*, jclass) { sv_seg_release(); }

jstring JNICALL n_catalog_load(JNIEnv* env, jclass, jstring manifest, jstring pubkey) {
    std::string m, k;
    if (manifest) {
        const char* c = env->GetStringUTFChars(manifest, nullptr);
        if (c) {
            m = c;
            env->ReleaseStringUTFChars(manifest, c);
        }
    }
    if (pubkey) {
        const char* c = env->GetStringUTFChars(pubkey, nullptr);
        if (c) {
            k = c;
            env->ReleaseStringUTFChars(pubkey, c);
        }
    }
    const jint rc = sv_catalog_load(m.empty() ? nullptr : m.c_str(),
                                    k.empty() ? nullptr : k.c_str());
    return env->NewStringUTF(rc == SV_OK ? sv_catalog_json() : sv_catalog_error());
}

jstring JNICALL n_catalog_error(JNIEnv* env, jclass) {
    return env->NewStringUTF(sv_catalog_error());
}

jint JNICALL n_catalog_verify(JNIEnv* env, jclass, jstring dir, jstring id) {
    std::string d, i;
    if (dir) {
        const char* c = env->GetStringUTFChars(dir, nullptr);
        if (c) {
            d = c;
            env->ReleaseStringUTFChars(dir, c);
        }
    }
    if (id) {
        const char* c = env->GetStringUTFChars(id, nullptr);
        if (c) {
            i = c;
            env->ReleaseStringUTFChars(id, c);
        }
    }
    char detail[256] = {0};
    return sv_catalog_verify_file(d.c_str(), i.c_str(), detail, sizeof(detail));
}

jstring JNICALL n_version(JNIEnv* env, jclass) { return env->NewStringUTF(sv_version()); }

jstring JNICALL n_log_dump(JNIEnv* env, jclass) {
    char buf[4096];
    const int32_t n = sv_log_dump(buf, sizeof(buf));
    (void)n;
    return env->NewStringUTF(buf);
}

const JNINativeMethod kMethods[] = {
    {const_cast<char*>("nativeInit"), const_cast<char*>("(J)I"), reinterpret_cast<void*>(n_init)},
    {const_cast<char*>("nativeLoadModel"), const_cast<char*>("(Ljava/lang/String;ZI)I"),
     reinterpret_cast<void*>(n_load)},
    {const_cast<char*>("nativeInpaint"), const_cast<char*>("([BII[BIII[I[J)I"),
     reinterpret_cast<void*>(n_inpaint)},
    {const_cast<char*>("nativeRelease"), const_cast<char*>("()V"), reinterpret_cast<void*>(n_release)},
    {const_cast<char*>("nativeVersion"), const_cast<char*>("()Ljava/lang/String;"),
     reinterpret_cast<void*>(n_version)},
    {const_cast<char*>("nativeLogDump"), const_cast<char*>("()Ljava/lang/String;"),
     reinterpret_cast<void*>(n_log_dump)},
    {const_cast<char*>("nativeRssMb"), const_cast<char*>("()D"),
     reinterpret_cast<void*>(n_rss_mb)},
    {const_cast<char*>("nativeSegLoad"), const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;)I"),
     reinterpret_cast<void*>(n_seg_load)},
    {const_cast<char*>("nativeSegPrepare"), const_cast<char*>("([BII)I"),
     reinterpret_cast<void*>(n_seg_prepare)},
    {const_cast<char*>("nativeSegTap"), const_cast<char*>("([I[I[BI[B[I)I"),
     reinterpret_cast<void*>(n_seg_tap)},
    {const_cast<char*>("nativeSegRelease"), const_cast<char*>("()V"),
     reinterpret_cast<void*>(n_seg_release)},
    {const_cast<char*>("nativeCatalogLoad"),
     const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"),
     reinterpret_cast<void*>(n_catalog_load)},
    {const_cast<char*>("nativeCatalogError"), const_cast<char*>("()Ljava/lang/String;"),
     reinterpret_cast<void*>(n_catalog_error)},
    {const_cast<char*>("nativeCatalogVerify"), const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;)I"),
     reinterpret_cast<void*>(n_catalog_verify)},
};

}  // namespace

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return -1;
    jclass cls = env->FindClass(kClass);
    if (!cls) return -1;  // the Kotlin class must be loadable before the library
    const int rc =
        env->RegisterNatives(cls, kMethods, sizeof(kMethods) / sizeof(kMethods[0]));
    env->DeleteLocalRef(cls);
    return rc == JNI_OK ? JNI_VERSION_1_6 : -1;
}
