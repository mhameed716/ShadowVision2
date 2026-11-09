// SPDX-License-Identifier: MIT
// CI-only stand-in for <android/log.h>, so that sv_c_api.cpp can be type-checked
// on a host without the NDK. Never shipped.
#pragma once

#define ANDROID_LOG_UNKNOWN 0
#define ANDROID_LOG_DEFAULT 1
#define ANDROID_LOG_VERBOSE 2
#define ANDROID_LOG_DEBUG 3
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_WARN 5
#define ANDROID_LOG_ERROR 6
#define ANDROID_LOG_FATAL 7

static inline int __android_log_print(int /*prio*/, const char* /*tag*/, const char* /*fmt*/, ...) {
    return 1;
}
