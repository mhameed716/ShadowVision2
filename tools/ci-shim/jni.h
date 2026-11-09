// SPDX-License-Identifier: MIT
// CI-only stand-in for <jni.h>.
//
// Purpose: type-check the JNI glue (android/app/src/main/cpp/jni_bridge.cpp) on a
// machine with no JDK and no NDK, so a typo in a JNI signature is caught by
// `tools/verify_g0.sh` instead of by a device crash at RegisterNatives time.
//
// It mirrors the real header's shape (JNIEnv/JavaVM are classes, so `env->X()`
// type-checks identically) but contains no ABI. Never shipped, never used when a
// real NDK is present.
#pragma once

#include <cstddef>
#include <cstdint>

typedef uint8_t jboolean;
typedef int8_t jbyte;
typedef uint16_t jchar;
typedef int16_t jshort;
typedef int32_t jint;
typedef int64_t jlong;
typedef float jfloat;
typedef double jdouble;
typedef jint jsize;

class _jobject {};
typedef _jobject* jobject;
typedef _jobject* jclass;
typedef _jobject* jstring;
typedef _jobject* jarray;
typedef _jobject* jbyteArray;
typedef _jobject* jintArray;
typedef _jobject* jlongArray;

#define JNI_VERSION_1_6 0x00010006
#define JNI_OK 0
#define JNI_ERR (-1)
#define JNIEXPORT
#define JNICALL

struct JNINativeMethod {
    const char* name;
    const char* signature;
    void* fnPtr;
};

namespace sv_ci_shim {
inline _jobject* object() {
    static _jobject o;
    return &o;
}
}  // namespace sv_ci_shim

class JNIEnv {
public:
    jclass FindClass(const char*) { return sv_ci_shim::object(); }
    jint ThrowNew(jclass, const char*) { return 0; }
    jsize GetArrayLength(jarray) { return 1 << 22; }
    const char* GetStringUTFChars(jstring, jboolean*) { return ""; }
    void ReleaseStringUTFChars(jstring, const char*) {}
    void* GetPrimitiveArrayCritical(jarray, jboolean*) { return buffer_; }
    void ReleasePrimitiveArrayCritical(jarray, void*, jint) {}
    void GetByteArrayRegion(jarray, jsize, jsize, jbyte* dst) { (void)dst; }
    void SetLongArrayRegion(jlongArray, jsize, jsize, const jlong*) {}
    jstring NewStringUTF(const char*) { return sv_ci_shim::object(); }
    jint RegisterNatives(jclass, const JNINativeMethod*, jint) { return JNI_OK; }
    void DeleteLocalRef(jobject) {}

private:
    unsigned char buffer_[64] = {};
};

class JavaVM {
public:
    jint GetEnv(void** out, jint) {
        static JNIEnv env;
        *out = &env;
        return JNI_OK;
    }
};
