# SPDX-License-Identifier: MIT
# JNI: RegisterNatives looks these up by exact name; R8 must not rename them.
-keep class ai.shadowvision.core.SvEngine { *; }
-keepclasseswithmembers,allowshrinking class ai.shadowvision.core.SvEngine {
    native <methods>;
}
-keep class ai.shadowvision.g0.MainActivity { *; }

# Compose + lifecycle are handled by consumer rules; nothing else custom is needed.
