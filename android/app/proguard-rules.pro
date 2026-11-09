# SPDX-License-Identifier: MIT
# JNI: RegisterNatives looks these up by exact name; R8 must not rename them.
-keep class ai.shadowvision.core.SvEngine { *; }
-keepclasseswithmembers,allowshrinking class ai.shadowvision.core.SvEngine {
    native <methods>;
}
-keep class ai.shadowvision.g0.MainActivity { *; }

# Compose + lifecycle are handled by consumer rules; nothing else custom is needed.

# ---------------------------------------------------------------------------
# R8 and the model paths.
#
# nativeCatalogVerify / sv_load_segmenter are handed a filesystem path built in
# Kotlin (Context.filesDir). Obfuscation does not change that path, but it DOES
# rename the classes whose simple names appear in native diagnostics and in the
# Java-side method table R8 rewrites for the registered natives. More importantly,
# minification was enabled with no mapping file kept, so a crash report from a
# release build is unreadable — and this app has no telemetry, so a crash report
# is the only evidence a user could ever send.
# ---------------------------------------------------------------------------

# Keep enough to make a stack trace usable, and keep the line numbers off (the
# source is not shipped, and the mapping file stays with the build).
-dontobfuscate

# The native layer registers by exact name on this class; keep the shell.
-keepclasseswithmembernames class ai.shadowvision.core.SvEngine {
    native <methods>;
}

# Compose runtime is reflection-light but its lambdas are numerous; the consumer
# rules handle it. Keep the app's own entry points only.
-keep class ai.shadowvision.studio.StudioActivity { *; }
