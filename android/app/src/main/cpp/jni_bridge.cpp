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

constexpr const char* kClass = "ai/shadowvision/g0/SvEngine";

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
    if (rgba_len < w * h * 4 || mask_len < mw * mh) {
        throw_illegal_state(env, "buffer sizes do not match the bitmap");
        return SV_ERR_INVALID_ARGUMENT;
    }

    auto* rgba_ptr = static_cast<uint8_t*>(env->GetPrimitiveArrayCritical(rgba, nullptr));
    if (!rgba_ptr) return SV_ERR_OUT_OF_MEMORY;
    std::vector<uint8_t> mask_copy(static_cast<size_t>(mw) * mh);
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
